#include <Processors/Transforms/MergingAggregatedMemoryEfficientTransform.h>

#include <Processors/ISimpleTransform.h>
#include <Interpreters/Aggregator.h>
#include <Processors/ResizeProcessor.h>

namespace DB
{

struct ChunksToMerge : public ChunkInfo
{
    std::unique_ptr<Chunks> chunks;
    Int32 bucket_num = -1;
    bool is_overflows = false;
};

GroupingAggregatedTransform::GroupingAggregatedTransform(
    const Block & header, size_t num_inputs, AggregatingTransformParamsPtr params)
    : IProcessor(InputPorts(num_inputs, header), {header})
    , num_inputs(num_inputs)
    , params(std::move(params))
    , last_bucket_number(num_inputs, -1)
    , read_from_input(num_inputs, false)
{
}

void GroupingAggregatedTransform::readFromAllInputs()
{
    auto in = inputs.begin();
    for (size_t i = 0; i < num_inputs; ++i, ++in)
    {
        if (in->isFinished())
            continue;

        if (read_from_input[i])
            continue;

        in->setNeeded();

        if (!in->hasData())
            return;

        auto chunk = in->pull();
        read_from_input[i] = true;
        addChunk(std::move(chunk), i);
    }

    read_from_all_inputs = true;
}

void GroupingAggregatedTransform::pushData(Chunks chunks, Int32 bucket, bool is_overflows)
{
    auto & output = outputs.front();

    auto info = std::make_shared<ChunksToMerge>();
    info->bucket_num = bucket;
    info->is_overflows = is_overflows;
    info->chunks = std::make_unique<Chunks>(std::move(chunks));

    Chunk chunk;
    chunk.setChunkInfo(std::move(info));
    output.push(std::move(chunk));
}

bool GroupingAggregatedTransform::tryPushTwoLevelData()
{
    for (; next_bucket_to_push < current_bucket || (all_inputs_finished && chunks.empty()); ++next_bucket_to_push)
    {
        auto batch_it = chunks.find(next_bucket_to_push);
        if (batch_it == chunks.end())
            continue;

        Chunks & cur_chunks = batch_it->second;
        if (cur_chunks.empty())
        {
            chunks.erase(batch_it);
            continue;
        }

        pushData(std::move(cur_chunks), current_bucket, false);
        chunks.erase(batch_it);
        return true;
    }

    return false;
}

bool GroupingAggregatedTransform::tryPushSingleLevelData()
{
    if (single_level_chunks.empty())
        return false;

    pushData(single_level_chunks, -1, false);
    return true;
}

bool GroupingAggregatedTransform::tryPushOverflowData()
{
    if (overflow_chunks.empty())
        return false;

    pushData(overflow_chunks, -1, true);
    return true;
}

IProcessor::Status GroupingAggregatedTransform::prepare()
{
    /// Check can output.
    auto & output = outputs.front();

    if (output.isFinished())
    {
        for (auto & input : inputs)
            input.close();

        chunks.clear();
        last_bucket_number.clear();
        return Status::Finished;
    }

    /// Read first time from each input to understand if we have two-level aggregation.
    if (!read_from_all_inputs)
    {
        readFromAllInputs();
        if (!read_from_all_inputs)
            return Status::NeedData;
    }

    /// Convert single level to two levels if have two-level input.
    if (has_two_level && !single_level_chunks.empty())
        return Status::Ready;

    /// Check can push (to avoid data caching).
    if (!output.canPush())
    {
        for (auto & input : inputs)
            input.setNotNeeded();

        return Status::PortFull;
    }

    bool pushed_to_output = false;

    /// Output if has data.
    if (has_two_level)
        pushed_to_output = tryPushTwoLevelData();

    auto need_input = [this](size_t input_num)
    {
        if (last_bucket_number[input_num] < current_bucket)
            return true;

        return expect_several_chunks_for_single_bucket_per_source && last_bucket_number[input_num] == current_bucket;
    };

    /// Read next bucket if can.
    for (; ; ++current_bucket)
    {
        bool finished = true;
        bool need_data = false;

        auto in = inputs.begin();
        for (size_t input_num = 0; input_num < num_inputs; ++input_num, ++in)
        {
            if (in->isFinished())
                continue;

            finished = false;

            if (!need_input(input_num))
                continue;

            in->setNeeded();

            if (!in->hasData())
            {
                need_data = true;
                continue;
            }

            auto chunk = in->pull();
            addChunk(std::move(chunk), input_num);

            if (has_two_level && !single_level_chunks.empty())
                return Status::Ready;

            if (need_input(input_num))
                need_data = true;
        }

        if (finished)
        {
            all_inputs_finished = true;
            break;
        }

        if (need_data)
            return Status::NeedData;
    }

    /// We have read some data. Now try to push something again.
    if (!pushed_to_output)
    {
        if (has_two_level)
            pushed_to_output = tryPushTwoLevelData();
        else
            pushed_to_output = tryPushSingleLevelData();
    }

    /// If we haven't pushed to output, then all data was read. Push overflows if have.
    if (!pushed_to_output)
        pushed_to_output = tryPushOverflowData();

    if (pushed_to_output)
        return Status::PortFull;

    output.finish();
    return Status::Finished;
}

void GroupingAggregatedTransform::addChunk(Chunk chunk, size_t input)
{
    auto & info = chunk.getChunkInfo();
    if (!info)
        throw Exception("Chunk info was not set for chunk in GroupingAggregatedTransform.", ErrorCodes::LOGICAL_ERROR);

    auto * agg_info = typeid_cast<const AggregatedChunkInfo *>(info.get());
    if (!agg_info)
        throw Exception("Chunk should have AggregatedChunkInfo in GroupingAggregatedTransform.", ErrorCodes::LOGICAL_ERROR);

    Int32 bucket = agg_info->bucket_num;
    bool is_overflows = agg_info->is_overflows;

    if (is_overflows)
        overflow_chunks.emplace_back(std::move(chunk));
    else if (bucket < 0)
        single_level_chunks.emplace_back(std::move(chunk));
    else
    {
        chunks[bucket].emplace_back(std::move(chunk));
        has_two_level = true;
        last_bucket_number[input] = bucket;
    }
}

void GroupingAggregatedTransform::work()
{
    if (!single_level_chunks.empty())
    {
        auto & header = getOutputs().front().getHeader();
        auto block = header.cloneWithColumns(single_level_chunks.back().detachColumns());
        single_level_chunks.pop_back();
        auto blocks = params->aggregator.convertBlockToTwoLevel(block);

        for (auto & cur_block : blocks)
        {
            Int32 bucket = cur_block.info.bucket_num;
            chunks[bucket].emplace_back(Chunk(cur_block.getColumns(), cur_block.rows()));
        }
    }
}


MergingAggregatedBucketTransform::MergingAggregatedBucketTransform(AggregatingTransformParamsPtr params)
    : ISimpleTransform({}, params->getHeader(), false), params(std::move(params))
{
    setInputNotNeededAfterRead(true);
}

void MergingAggregatedBucketTransform::transform(Chunk & chunk)
{
    auto & info = chunk.getChunkInfo();
    auto * chunks_to_merge = typeid_cast<const ChunksToMerge *>(info.get());

    if (!chunks_to_merge)
        throw Exception("MergingAggregatedSimpleTransform chunk must have ChunkInfo with type ChunksToMerge.",
                        ErrorCodes::LOGICAL_ERROR);

    BlocksList blocks_list;
    for (auto & cur_chunk : *chunks_to_merge->chunks)
        blocks_list.emplace_back(getInputPort().getHeader().cloneWithColumns(cur_chunk.detachColumns()));

    chunk.setChunkInfo(nullptr);

    auto block = params->aggregator.mergeBlocks(blocks_list, params->final);
    size_t num_rows = block.rows();
    chunk.setColumns(block.getColumns(), num_rows);
}


SortingAggregatedTransform::SortingAggregatedTransform(size_t num_inputs, AggregatingTransformParamsPtr params)
    : IProcessor(InputPorts(num_inputs, params->getHeader()), {params->getHeader()})
    , num_inputs(num_inputs)
    , params(std::move(params))
{

}

}
