#include <Storages/MergeTree/MergeTreeBlockReadUtils.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Common/typeid_cast.h>
#include <Columns/ColumnConst.h>
#include <unordered_set>


namespace DB
{

NameSet injectRequiredColumns(const MergeTreeData & storage, const MergeTreeData::DataPartPtr & part, Names & columns)
{
    NameSet required_columns{std::begin(columns), std::end(columns)};
    NameSet injected_columns;

    auto all_column_files_missing = true;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto & column_name = columns[i];

        /// column has files and hence does not require evaluation
        if (part->hasColumnFiles(column_name, *storage.getColumn(column_name).type))
        {
            all_column_files_missing = false;
            continue;
        }

        const auto column_default = storage.getColumns().getDefault(column_name);
        if (!column_default)
            continue;

        /// collect identifiers required for evaluation
        IdentifierNameSet identifiers;
        column_default->expression->collectIdentifierNames(identifiers);

        for (const auto & identifier : identifiers)
        {
            if (storage.hasColumn(identifier))
            {
                /// ensure each column is added only once
                if (required_columns.count(identifier) == 0)
                {
                    columns.emplace_back(identifier);
                    required_columns.emplace(identifier);
                    injected_columns.emplace(identifier);
                }
            }
        }
    }

    /** Add a column of the minimum size.
        * Used in case when no column is needed or files are missing, but at least you need to know number of rows.
        * Adds to the columns.
        */
    if (all_column_files_missing)
    {
        const auto minimum_size_column_name = part->getColumnNameWithMinumumCompressedSize();
        columns.push_back(minimum_size_column_name);
        /// correctly report added column
        injected_columns.insert(columns.back());
    }

    return injected_columns;
}


MergeTreeReadTask::MergeTreeReadTask(
    const MergeTreeData::DataPartPtr & data_part_, const MarkRanges & mark_ranges_, const size_t part_index_in_query_,
    const Names & ordered_names_, const NameSet & column_name_set_, const NamesAndTypesList & columns_,
    const NamesAndTypesList & pre_columns_, const bool remove_prewhere_column_, const bool should_reorder_,
    MergeTreeBlockSizePredictorPtr && size_predictor_)
    : data_part{data_part_}, mark_ranges{mark_ranges_}, part_index_in_query{part_index_in_query_},
    ordered_names{ordered_names_}, column_name_set{column_name_set_}, columns{columns_}, pre_columns{pre_columns_},
    remove_prewhere_column{remove_prewhere_column_}, should_reorder{should_reorder_}, size_predictor{std::move(size_predictor_)}
{
}

MergeTreeReadTask::~MergeTreeReadTask() = default;


MergeTreeBlockSizePredictor::MergeTreeBlockSizePredictor(
    const MergeTreeData::DataPartPtr & data_part_, const Names & columns, const Block & sample_block)
    : data_part(data_part_)
{
    number_of_rows_in_part = data_part->rows_count;
    /// Initialize with sample block until update won't called.
    initialize(sample_block, {}, columns);
}

void MergeTreeBlockSizePredictor::initialize(const Block & sample_block, const Columns & columns, const Names & names, bool from_update)
{
    fixed_columns_bytes_per_row = 0;
    dynamic_columns_infos.clear();

    std::unordered_set<String> names_set;
    if (!from_update)
        names_set.insert(names.begin(), names.end());

    size_t num_columns = sample_block.columns();
    for (size_t pos = 0; pos < num_columns; ++pos)
    {
        const auto & column_with_type_and_name = sample_block.getByPosition(pos);
        const String & column_name = column_with_type_and_name.name;
        const ColumnPtr & column_data = from_update ? columns[pos]
                                                    : column_with_type_and_name.column;

        if (!from_update && !names_set.count(column_name))
            continue;

        /// At least PREWHERE filter column might be const.
        if (typeid_cast<const ColumnConst *>(column_data.get()))
            continue;

        if (column_data->valuesHaveFixedSize())
        {
            size_t size_of_value = column_data->sizeOfValueIfFixed();
            fixed_columns_bytes_per_row += column_data->sizeOfValueIfFixed();
            max_size_per_row_fixed = std::max<size_t>(max_size_per_row_fixed, size_of_value);
        }
        else
        {
            ColumnInfo info;
            info.name = column_name;
            /// If column isn't fixed and doesn't have checksum, than take first
            ColumnSize column_size = data_part->getColumnSize(
                column_name, *column_with_type_and_name.type);

            info.bytes_per_row_global = column_size.data_uncompressed
                ? column_size.data_uncompressed / number_of_rows_in_part
                : column_data->byteSize() / std::max<size_t>(1, column_data->size());

            dynamic_columns_infos.emplace_back(info);
        }
    }

    bytes_per_row_global = fixed_columns_bytes_per_row;
    for (auto & info : dynamic_columns_infos)
    {
        info.bytes_per_row = info.bytes_per_row_global;
        bytes_per_row_global += info.bytes_per_row_global;

        max_size_per_row_dynamic = std::max<double>(max_size_per_row_dynamic, info.bytes_per_row);
    }
    bytes_per_row_current = bytes_per_row_global;
}

void MergeTreeBlockSizePredictor::startBlock()
{
    block_size_bytes = 0;
    block_size_rows = 0;
    for (auto & info : dynamic_columns_infos)
        info.size_bytes = 0;
}


/// TODO: add last_read_row_in_part parameter to take into account gaps between adjacent ranges
void MergeTreeBlockSizePredictor::update(const Block & sample_block, const Columns & columns, size_t num_rows, double decay)
{
    if (columns.size() != sample_block.columns())
        throw Exception("Inconsistent number of columns passed to MergeTreeBlockSizePredictor. "
                        "Have " + toString(sample_block.columns()) + " in sample block "
                        "and " + toString(columns.size()) + " columns in list", ErrorCodes::LOGICAL_ERROR);

    if (!is_initialized_in_update)
    {
        /// Reinitialize with read block to update estimation for DEFAULT and MATERIALIZED columns without data.
        initialize(sample_block, columns, {}, true);
        is_initialized_in_update = true;
    }

    if (num_rows < block_size_rows)
    {
        throw Exception("Updated block has less rows (" + toString(num_rows) + ") than previous one (" + toString(block_size_rows) + ")",
                        ErrorCodes::LOGICAL_ERROR);
    }

    size_t diff_rows = num_rows - block_size_rows;
    block_size_bytes = num_rows * fixed_columns_bytes_per_row;
    bytes_per_row_current = fixed_columns_bytes_per_row;
    block_size_rows = num_rows;

    /// Make recursive updates for each read row: v_{i+1} = (1 - decay) v_{i} + decay v_{target}
    /// Use sum of geometric sequence formula to update multiple rows: v{n} = (1 - decay)^n v_{0} + (1 - (1 - decay)^n) v_{target}
    /// NOTE: DEFAULT and MATERIALIZED columns without data has inaccurate estimation of v_{target}
    double alpha = std::pow(1. - decay, diff_rows);

    max_size_per_row_dynamic = 0;
    for (auto & info : dynamic_columns_infos)
    {
        size_t new_size = columns[sample_block.getPositionByName(info.name)]->byteSize();
        size_t diff_size = new_size - info.size_bytes;

        double local_bytes_per_row = static_cast<double>(diff_size) / diff_rows;
        info.bytes_per_row = alpha * info.bytes_per_row + (1. - alpha) * local_bytes_per_row;

        info.size_bytes = new_size;
        block_size_bytes += new_size;
        bytes_per_row_current += info.bytes_per_row;

        max_size_per_row_dynamic = std::max<double>(max_size_per_row_dynamic, info.bytes_per_row);
    }
}


MergeTreeReadTaskColumns getReadTaskColumns(const MergeTreeData & storage, const MergeTreeData::DataPartPtr & data_part,
    const Names & required_columns, const PrewhereInfoPtr & prewhere_info, bool check_columns)
{
    Names column_names = required_columns;
    Names pre_column_names;

    /// inject columns required for defaults evaluation
    bool should_reorder = !injectRequiredColumns(storage, data_part, column_names).empty();

    if (prewhere_info)
    {
        if (prewhere_info->alias_actions)
            pre_column_names = prewhere_info->alias_actions->getRequiredColumns();
        else
            pre_column_names = prewhere_info->prewhere_actions->getRequiredColumns();

        if (pre_column_names.empty())
            pre_column_names.push_back(column_names[0]);

        const auto injected_pre_columns = injectRequiredColumns(storage, data_part, pre_column_names);
        if (!injected_pre_columns.empty())
            should_reorder = true;

        const NameSet pre_name_set(pre_column_names.begin(), pre_column_names.end());

        Names post_column_names;
        for (const auto & name : column_names)
            if (!pre_name_set.count(name))
                post_column_names.push_back(name);

        column_names = post_column_names;
    }

    MergeTreeReadTaskColumns result;

    if (check_columns)
    {
        /// Under owned_data_part->columns_lock we check that all requested columns are of the same type as in the table.
        /// This may be not true in case of ALTER MODIFY.
        if (!pre_column_names.empty())
            storage.check(data_part->columns, pre_column_names);
        if (!column_names.empty())
            storage.check(data_part->columns, column_names);

        const NamesAndTypesList & physical_columns = storage.getColumns().getAllPhysical();
        result.pre_columns = physical_columns.addTypes(pre_column_names);
        result.columns = physical_columns.addTypes(column_names);
    }
    else
    {
        result.pre_columns = data_part->columns.addTypes(pre_column_names);
        result.columns = data_part->columns.addTypes(column_names);
    }

    result.should_reorder = should_reorder;

    return result;
}

}