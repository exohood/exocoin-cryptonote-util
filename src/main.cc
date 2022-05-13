#include <cmath>
#include <node.h>
#include <node_buffer.h>
#include <v8.h>
#include <stdint.h>
#include <string>
#include <algorithm>
#include "cryptonote_core/cryptonote_basic.h"
#include "cryptonote_core/cryptonote_format_utils.h"
#include "cryptonote_protocol/blobdatatype.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "common/base58.h"
#include "serialization/binary_utils.h"
#include <nan.h>

#define THROW_ERROR_EXCEPTION(x) NanThrowError(x)
#define THROW_ERROR_EXCEPTION_WITH_STATUS_CODE(x, y) NanThrowError(x, y)
        
using namespace node;
using namespace v8;
using namespace cryptonote;

blobdata uint64be_to_blob(uint64_t num) {
    blobdata res = "        ";
    res[0] = num >> 56 & 0xff;
    res[1] = num >> 48 & 0xff;
    res[2] = num >> 40 & 0xff;
    res[3] = num >> 32 & 0xff;
    res[4] = num >> 24 & 0xff;
    res[5] = num >> 16 & 0xff;
    res[6] = num >> 8  & 0xff;
    res[7] = num       & 0xff;
    return res;
}


static bool fillExtra(cryptonote::block& block1, const cryptonote::block& block2) {
    cryptonote::tx_extra_merge_mining_tag mm_tag;
    mm_tag.depth = 0;
    if (!cryptonote::get_block_header_hash(block2, mm_tag.merkle_root))
        return false;

    block1.miner_tx.extra.clear();
    if (!cryptonote::append_mm_tag_to_extra(block1.miner_tx.extra, mm_tag))
        return false;

    return true;
}

static bool mergeBlocks(const cryptonote::block& block1, cryptonote::block& block2, const std::vector<crypto::hash>& branch2) {
    block2.timestamp = block1.timestamp;
    block2.parent_block.major_version = block1.major_version;
    block2.parent_block.minor_version = block1.minor_version;
    block2.parent_block.prev_id = block1.prev_id;
    block2.parent_block.nonce = block1.nonce;
    block2.parent_block.miner_tx = block1.miner_tx;
    block2.parent_block.number_of_transactions = block1.tx_hashes.size() + 1;
    block2.parent_block.miner_tx_branch.resize(crypto::tree_depth(block1.tx_hashes.size() + 1));
    std::vector<crypto::hash> transactionHashes;
    transactionHashes.push_back(cryptonote::get_transaction_hash(block1.miner_tx));
    std::copy(block1.tx_hashes.begin(), block1.tx_hashes.end(), std::back_inserter(transactionHashes));
    tree_branch(transactionHashes.data(), transactionHashes.size(), block2.parent_block.miner_tx_branch.data());
    block2.parent_block.blockchain_branch = branch2;
    return true;
}

static bool construct_parent_block(const cryptonote::block& b, cryptonote::block& parent_block) {
    parent_block.major_version = 1;
    parent_block.minor_version = 0;
    parent_block.timestamp = b.timestamp;
    parent_block.prev_id = b.prev_id;
    parent_block.nonce = b.parent_block.nonce;
    parent_block.miner_tx.version = CURRENT_TRANSACTION_VERSION;
    parent_block.miner_tx.unlock_time = 0;

    return fillExtra(parent_block, b);
}

NAN_METHOD(convert_blob) {
    NanScope();

    if (args.Length() < 1)
        return THROW_ERROR_EXCEPTION("You must provide one argument.");

    Local<Object> target = args[0]->ToObject();

    if (!Buffer::HasInstance(target))
        return THROW_ERROR_EXCEPTION("Argument should be a buffer object.");

    blobdata input = std::string(Buffer::Data(target), Buffer::Length(target));
    blobdata output = "";

    //convert
    block b = AUTO_VAL_INIT(b);
    if (!parse_and_validate_block_from_blob(input, b))
        return THROW_ERROR_EXCEPTION("Failed to parse block");

    if (b.major_version < BLOCK_MAJOR_VERSION_2) {
        if (!get_block_hashing_blob(b, output))
            return THROW_ERROR_EXCEPTION("Failed to create mining block");
    } else {
        block parent_block;
        if (!construct_parent_block(b, parent_block))
            return THROW_ERROR_EXCEPTION("Failed to construct parent block");

        if (!get_block_hashing_blob(parent_block, output))
            return THROW_ERROR_EXCEPTION("Failed to create mining block");
    }

    NanReturnValue(
        NanNewBufferHandle(output.data(), output.size())
    );
}

NAN_METHOD(get_block_id) {
    NanScope();

    if (args.Length() < 1)
        return THROW_ERROR_EXCEPTION("You must provide one argument.");

    Local<Object> target = args[0]->ToObject();

    if (!Buffer::HasInstance(target))
        return THROW_ERROR_EXCEPTION("Argument should be a buffer object.");

    blobdata input = std::string(Buffer::Data(target), Buffer::Length(target));
    blobdata output = "";

    block b = AUTO_VAL_INIT(b);
    if (!parse_and_validate_block_from_blob(input, b))
        return THROW_ERROR_EXCEPTION("Failed to parse block");

    crypto::hash block_id;
    if (!get_block_hash(b, block_id))
        return THROW_ERROR_EXCEPTION("Failed to calculate hash for block");

    NanReturnValue(
        NanNewBufferHandle(reinterpret_cast<char*>(&block_id), sizeof(block_id))
    );
}

NAN_METHOD(construct_block_blob) {
    NanScope();

    if (args.Length() < 2)
        return THROW_ERROR_EXCEPTION("You must provide two arguments.");

    Local<Object> block_template_buf = args[0]->ToObject();
    Local<Object> nonce_buf = args[1]->ToObject();

    if (!Buffer::HasInstance(block_template_buf) || !Buffer::HasInstance(nonce_buf))
        return THROW_ERROR_EXCEPTION("Both arguments should be buffer objects.");

    if (Buffer::Length(nonce_buf) != 4)
        return THROW_ERROR_EXCEPTION("Nonce buffer has invalid size.");

    uint32_t nonce = *reinterpret_cast<uint32_t*>(Buffer::Data(nonce_buf));

    blobdata block_template_blob = std::string(Buffer::Data(block_template_buf), Buffer::Length(block_template_buf));
    blobdata output = "";

    block b = AUTO_VAL_INIT(b);
    if (!parse_and_validate_block_from_blob(block_template_blob, b))
        return THROW_ERROR_EXCEPTION("Failed to parse block");

    b.nonce = nonce;
    if (b.major_version == BLOCK_MAJOR_VERSION_2) {
        block parent_block;
        b.parent_block.nonce = nonce;
        if (!construct_parent_block(b, parent_block))
            return THROW_ERROR_EXCEPTION("Failed to construct parent block");

        if (!mergeBlocks(parent_block, b, std::vector<crypto::hash>()))
            return THROW_ERROR_EXCEPTION("Failed to postprocess mining block");
    }

    if (!block_to_blob(b, output))
        return THROW_ERROR_EXCEPTION("Failed to convert block to blob");

    NanReturnValue(
        NanNewBufferHandle(output.data(), output.size())
    );
}

NAN_METHOD(convert_blob_bb) {
    NanScope();

    if (args.Length() < 1)
        return THROW_ERROR_EXCEPTION("You must provide one argument.");

    Local<Object> target = args[0]->ToObject();

    if (!Buffer::HasInstance(target))
        return THROW_ERROR_EXCEPTION("Argument should be a buffer object.");

    blobdata input = std::string(Buffer::Data(target), Buffer::Length(target));
    blobdata output = "";

    //convert
    bb_block b = AUTO_VAL_INIT(b);
    if (!parse_and_validate_block_from_blob(input, b)) {
        return THROW_ERROR_EXCEPTION("Failed to parse block");
    }
    output = get_block_hashing_blob(b);

    NanReturnValue(
        NanNewBufferHandle(output.data(), output.size())
    );
}

NAN_METHOD(address_decode) {
    NanEscapableScope();

    if (args.Length() < 1)
        return THROW_ERROR_EXCEPTION("You must provide one argument.");

    Local<Object> target = args[0]->ToObject();

    if (!Buffer::HasInstance(target))
        return THROW_ERROR_EXCEPTION("Argument should be a buffer object.");

    blobdata input = std::string(Buffer::Data(target), Buffer::Length(target));

    blobdata data;
    uint64_t prefix;
    if (!tools::base58::decode_addr(input, prefix, data))
        NanReturnUndefined();

    account_public_address adr;
    if (!::serialization::parse_binary(data, adr))
        NanReturnUndefined();

    if (!crypto::check_key(adr.m_spend_public_key) || !crypto::check_key(adr.m_view_public_key))
        NanReturnUndefined();

    NanReturnValue(NanNew(static_cast<uint32_t>(prefix)));
}

void init(Handle<Object> exports) {
    exports->Set(NanNew<String>("construct_block_blob"), NanNew<FunctionTemplate>(construct_block_blob)->GetFunction());
    exports->Set(NanNew<String>("get_block_id"), NanNew<FunctionTemplate>(get_block_id)->GetFunction());
    exports->Set(NanNew<String>("convert_blob"), NanNew<FunctionTemplate>(convert_blob)->GetFunction());
    exports->Set(NanNew<String>("convert_blob_bb"), NanNew<FunctionTemplate>(convert_blob_bb)->GetFunction());
    exports->Set(NanNew<String>("address_decode"), NanNew<FunctionTemplate>(address_decode)->GetFunction());
}

NODE_MODULE(cryptonote, init)
