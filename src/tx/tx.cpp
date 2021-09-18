// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <algorithm>

#include "tx.h"
#include "persistence/accountdb.h"
#include "persistence/contractdb.h"
#include "persistence/txdb.h"
#include "entities/account.h"
#include "commons/json/json_spirit_value.h"
#include "commons/json/json_spirit_writer_template.h"
#include "commons/json/json_spirit_utils.h"
#include "commons/serialize.h"
#include "crypto/hash.h"
#include "commons/util/util.h"
#include "main.h"
#include "vm/luavm/luavmrunenv.h"
#include "miner/miner.h"
#include "config/version.h"

using namespace json_spirit;
extern CCacheDBManager *pCdMan;


#define ERROR_TITLE(msg) (std::string(__FUNCTION__) + "(), " + msg)
#define BASE_TX_TITLE ERROR_TITLE(GetTxTypeName())

string GetTxType(const TxType txType) {
    auto it = kTxTypeInfoTable.find(txType);
    if (it != kTxTypeInfoTable.end())
        return std::get<0>(it->second);
    else
        return "";
}

TxType ParseTxType(const string &str) {
    for (const auto &item : kTxTypeInfoTable) {
        if (std::get<0>(item.second) == str)
            return item.first;
    }
    return TxType::NULL_TX;
}

bool GetTxMinFee(CCacheWrapper &cw, const TxType nTxType, int height, const TokenSymbol &symbol, uint64_t &feeOut) {
    if (cw.sysParamCache.GetMinerFee(nTxType, symbol, feeOut))
        return true;

    const auto &iter = kTxTypeInfoTable.find(nTxType);
    if (iter != kTxTypeInfoTable.end()) {
        FeatureForkVersionEnum version = GetFeatureForkVersion(height);
        if (symbol == SYMB::WICC) {
            if (version >= MAJOR_VER_R2) {
                feeOut = std::get<2>(iter->second);
                return true;
            } else { //MAJOR_VER_R1  //Prior-stablecoin Release
                feeOut = std::get<1>(iter->second);
                return true;
            }
        } else if (symbol == SYMB::WUSD) {
            if (version >= MAJOR_VER_R2){
                feeOut = std::get<4>(iter->second);
                return true;
            } { //MAJOR_VER_R1 //Prior-stablecoin Release
                feeOut = std::get<3>(iter->second);
                return true;
            }
        }
    }
    return false;
}

bool CBaseTx::IsValidHeight(int32_t nCurrHeight, int32_t nTxCacheHeight) const {
    if (BLOCK_REWARD_TX == nTxType || UCOIN_BLOCK_REWARD_TX == nTxType || PRICE_MEDIAN_TX == nTxType)
        return true;

    auto halfRange = nTxCacheHeight / 2;
    return (valid_height <= nCurrHeight + halfRange) && (valid_height >= nCurrHeight - halfRange);
}

uint64_t CBaseTx::GetFuelFee(CCacheWrapper &cw, int32_t height, uint32_t fuelRate) {
    return (fuel == 0 || fuelRate == 0) ? 0 : std::ceil(fuel / 100.0f) * fuelRate;
}

bool CBaseTx::CheckBaseTx(CTxExecuteContext &context) {
    CValidationState &state = *context.pState;
    ClearMemData();

    if(    nTxType == BLOCK_REWARD_TX
        || nTxType == PRICE_MEDIAN_TX
        || nTxType == UCOIN_MINT_TX
        || nTxType == UCOIN_BLOCK_REWARD_TX
        || nTxType == CDP_FORCE_SETTLE_INTEREST_TX )
        return true;

    sp_tx_account = GetAccount(context, txUid, "txUid");
    if (!sp_tx_account) return false;

    { //1. Tx signature check
        bool signatureValid = false;
        if (GetFeatureForkVersion(context.height) < MAJOR_VER_R2) {
            signatureValid = true; //due to a pre-existing bug and illegally issued unsigned vote Tx
        } else {
            CPubKey pubKey;
            if (txUid.is<CPubKey>()) {
                pubKey = txUid.get<CPubKey>();
            } else {
                if (sp_tx_account->perms_sum == 0) {
                    return state.DoS(100, ERRORMSG("perms_sum is zero error! txUid=%s",
                                txUid.ToString()), READ_ACCOUNT_FAIL, "bad-tx-sign");
                }
                if (!sp_tx_account->IsRegistered())
                    return state.DoS(100, ERRORMSG("tx account was not registered! txUid=%s",
                                txUid.ToString()), READ_ACCOUNT_FAIL, "tx-account-not-registered");

                pubKey = sp_tx_account->owner_pubkey;
            }

            signatureValid = VerifySignature(context, pubKey);
        }

        if (!signatureValid)
            return state.DoS(100, ERRORMSG("verify txUid %s sign failed", txUid.ToString()),
                            READ_ACCOUNT_FAIL, "bad-tx-sign");
    }

    { //2. check Tx fee
        switch (nTxType) {
            case LCONTRACT_DEPLOY_TX:
            case LCONTRACT_INVOKE_TX: break; //to be checked in Tx Code but not here
            default:
                if(!CheckFee(context)) return false;
        }
    }

    {//3. check Tx RegID or PubKey
        switch (nTxType) {
            case ACCOUNT_REGISTER_TX: break; // will check txUid in CheckTx()
            case LCONTRACT_DEPLOY_TX:
            case ASSET_ISSUE_TX:
            case UCONTRACT_DEPLOY_TX:
            case PRICE_FEED_TX:
            case DEX_TRADE_SETTLE_TX:
            case PROPOSAL_APPROVAL_TX: IMPLEMENT_CHECK_TX_REGID(txUid); break;
            default: IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid); break;
        }
    }

    { //4. check tx is available by soft-fork version, for testnet/regtest backward compatibility
        switch (nTxType) {
            case ACCOUNT_REGISTER_TX:
            case BCOIN_TRANSFER_TX:
            case LCONTRACT_DEPLOY_TX:
            case LCONTRACT_INVOKE_TX:
            case DELEGATE_VOTE_TX: break; // tx available from MAJOR_VER_R1

            default: {
                auto itr = kTxTypeInfoTable.find(nTxType);
                if (!CheckTxAvailableFromVer(context, std::get<6>(itr->second))) return false;
            }

        }
    }

    { //5. check account perm
        if (kTxTypePermMap.find(nTxType) == kTxTypePermMap.end())
            return true; //no perm required

        if (sp_tx_account->perms_sum == 0 || (sp_tx_account->perms_sum & kTxTypePermMap.at(nTxType)) == 0)
            return state.DoS(100, ERRORMSG("account (%s) has NO required perm",
                                           txUid.ToString()), READ_ACCOUNT_FAIL, "account-lacks-perm");
    }

    return true;
}

bool CBaseTx::ExecuteFullTx(CTxExecuteContext &context) {
    auto bm = MAKE_BENCHMARK("ExecuteFullTx");
    IMPLEMENT_DEFINE_CW_STATE;
    ClearMemData();
    txCord = CTxCord(context.height, context.index);

    bool processingTxAccount = (nTxType != PRICE_MEDIAN_TX) && (nTxType != UCOIN_MINT_TX) && (nTxType != CDP_FORCE_SETTLE_INTEREST_TX);

    /////////////////////////
    // 1. Prior ExecuteTx
    if (processingTxAccount) {
        sp_tx_account = GetAccount(context, txUid, "txUid");
        if (!sp_tx_account) return false;

        if (!RegisterAccountPubKey(context)) {
            return false; // error msg has been processed
        }

        if (nTxType != UCOIN_BLOCK_REWARD_TX && nTxType != BLOCK_REWARD_TX) {
            if (llFees > 0 && !sp_tx_account->OperateBalance(fee_symbol, SUB_FREE, llFees, ReceiptType::BLOCK_REWARD_TO_MINER, receipts))
                    return state.DoS(100, ERRORMSG("ExecuteFullTx: account has insufficient funds"),
                                    UPDATE_ACCOUNT_FAIL, "sub-account-fees-failed");
        }
    }

    /////////////////////////
    // 2. ExecuteTx
    if (!ExecuteTx(context))
        return false;

    /////////////////////////
    // 3. Post ExecuteTx
    if (!SaveAllAccounts(context)) return false;

    if (!receipts.empty() && !cw.txReceiptCache.SetTxReceipts(GetHash(), receipts))
        return state.DoS(100, ERRORMSG("ExecuteFullTx: save receipts error, txid=%s",
                                    GetHash().ToString()), WRITE_RECEIPT_FAIL, "bad-save-receipts");

    return true;
}

void CBaseTx::ClearMemData() {
    account_map.clear();
    sp_tx_account = nullptr;
    receipts.clear();
}

bool CBaseTx::CheckTxFeeSufficient(CCacheWrapper &cw, const TokenSymbol &feeSymbol, const uint64_t llFees, const int32_t height) const {
    uint64_t minFee;
    if (!GetTxMinFee(cw, nTxType, height, feeSymbol, minFee)) {
        assert(false && "Get tx min fee for WICC or WUSD");
        return false;
    }
    return llFees >= minFee;
}
string CBaseTx::ToString(CAccountDBCache &accountCache) {
    return  strprintf("txType=%s", GetTxType(nTxType)) + ", " +
            strprintf("hash=%s", GetHash().GetHex()) + ", " +
            strprintf("ver=%d", nVersion) + ", " +
            strprintf("valid_height=%d", valid_height) + ", " +
            strprintf("tx_uid=%s", txUid.ToDebugString()) + ", " +
            strprintf("fee_symbol=%llu", fee_symbol) + ", " +
            strprintf("fees=%llu", llFees) + ", " +
            strprintf("signature=%s", HexStr(signature));
}

Object CBaseTx::ToJson(CCacheWrapper &cw) const {
    Object result;
    CKeyID srcKeyId;
    cw.accountCache.GetKeyId(txUid, srcKeyId);
    result.push_back(Pair("txid",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("tx_uid",         txUid.ToString()));
    result.push_back(Pair("from_addr",      srcKeyId.ToAddress()));
    result.push_back(Pair("fee_symbol",     fee_symbol));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("valid_height",   valid_height));
    result.push_back(Pair("signature",      HexStr(signature)));
    return result;
}

bool CBaseTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    return AddInvolvedKeyIds({txUid}, cw, keyIds);
}

bool CBaseTx::AddInvolvedKeyIds(vector<CUserID> uids, CCacheWrapper &cw, set<CKeyID> &keyIds) {
    for (auto uid : uids) {
        CKeyID keyId;
        if (!cw.accountCache.GetKeyId(uid, keyId))
            return false;

        keyIds.insert(keyId);
    }
    return true;
}

shared_ptr<CAccount> CBaseTx::GetAccount(CTxExecuteContext &context, const CUserID &uid,
                                         const string &name) {
    shared_ptr<CAccount> spAccount = GetAccount(*context.pCw, uid);
    if (!spAccount) {
        context.pState->DoS(100, ERRORMSG("%s, %s account not exist, uid=%s", GetTxTypeName(), name, uid.ToString()),
                                REJECT_INVALID, "account-not-exist");
        return nullptr;
    }
    return spAccount;
}

shared_ptr<CAccount> CBaseTx::GetAccount(CCacheWrapper &cw, const CUserID &uid) {
    if (sp_tx_account && !sp_tx_account->IsEmpty() && sp_tx_account->IsSelfUid(uid)) {
        return sp_tx_account;
    }

    shared_ptr<CAccount> spAccount = nullptr;
    CKeyID keyid;
    if (!cw.accountCache.GetKeyId(uid, keyid)) {
        return nullptr;
    }
    auto it = account_map.find(keyid);
    if (it != account_map.end()) {
        spAccount = it->second;
    } else {
        spAccount = make_shared<CAccount>();
        if (!cw.accountCache.GetAccount(keyid, *spAccount)) {
            return nullptr;
        }
        account_map.emplace(keyid, spAccount);
    }
    return spAccount;
}

shared_ptr<CAccount> CBaseTx::NewAccount(CCacheWrapper &cw, const CKeyID &keyid) {
    shared_ptr<CAccount> spAccount = make_shared<CAccount>(keyid);
    account_map.emplace(spAccount->keyid, spAccount);
    return spAccount;
}

bool CBaseTx::SaveAllAccounts(CTxExecuteContext &context) {
    for (auto item : account_map) {
        if (!context.pCw->accountCache.SaveAccount(*item.second))
                return context.pState->DoS(100, ERRORMSG("write addr %s account info error",
                                item.first.ToAddress()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }
    return true;
}

bool CBaseTx::RegisterAccountPubKey(CTxExecuteContext &context) {
    if (!txUid.is<CPubKey>())
        return true;

    if (sp_tx_account->IsRegistered())
        return true;

    const CPubKey &pubkey = txUid.get<CPubKey>(); // init owner pubkey
    return RegisterAccount(context, &pubkey, *sp_tx_account);
}

bool CBaseTx::RegisterAccount(CTxExecuteContext &context, const CPubKey *pPubkey,
                              CAccount &account) {
    account.regid = CRegID(context.height, context.index); // generate new regid for account
    if (pPubkey != nullptr)
        account.owner_pubkey = *pPubkey; // init owner pubkey

    if (!context.pCw->accountCache.NewRegId(account.regid, account.keyid)) {
        return context.pState->DoS(100, ERRORMSG("save new regid failed! regid=%s, addr=%s",
                    account.regid.ToString(), account.keyid.ToAddress()),
                    READ_ACCOUNT_FAIL, "save-new-regid-failed");
    }
    return true;
}

bool CBaseTx::CheckFee(CTxExecuteContext &context) {
    // check fee value range
    if (!CheckBaseCoinRange(llFees))
        return context.pState->DoS(100, ERRORMSG("tx fee out of range"), REJECT_INVALID,
                         "bad-tx-fee-toolarge");
    // check fee symbol valid
    if (!kFeeSymbolSet.count(fee_symbol))
        return context.pState->DoS(100,
                         ERRORMSG("not support fee symbol=%s, only supports:%s", fee_symbol, GetFeeSymbolSetStr()),
                         REJECT_INVALID, "bad-tx-fee-symbol");

    uint64_t minFee;
    if (!GetTxMinFee(*context.pCw, nTxType, context.height, fee_symbol, minFee))
        return context.pState->DoS(100, ERRORMSG("GetTxMinFee failed, tx=%s", GetTxTypeName()),
            REJECT_INVALID, "get-tx-min-fee-failed");

    if (!CheckMinFee(context, minFee)) return false;

    return true;
}

bool CBaseTx::CheckMinFee(CTxExecuteContext &context, uint64_t minFee) {
    if (llFees < minFee){
        string err = strprintf("The given fee is too small: %llu < %llu sawi", llFees, minFee);
        return context.pState->DoS(100, ERRORMSG("%s, tx=%s, height=%d, fee_symbol=%s",
            err, GetTxTypeName(), context.height, fee_symbol), REJECT_INVALID, err);
    }
    return true;
}

bool CBaseTx::CheckTxAvailableFromVer(CTxExecuteContext &context, FeatureForkVersionEnum ver) {
    if (GetFeatureForkVersion(context.height) < ver)
        return context.pState->DoS(100, ERRORMSG("[%d]tx type=%s is unavailable before height=%d",
                context.height, GetTxTypeName(), GetForkHeightByVersion(ver)),
                REJECT_INVALID, "unavailable-tx");
    return true;
}

bool CBaseTx::VerifySignature(CTxExecuteContext &context, const CPubKey &pubkey) {
    uint256 sighash = GetHash();
    if (!::VerifySignature(sighash, signature, pubkey))
        return context.pState->DoS(100, ERRORMSG("%s, tx signature error", BASE_TX_TITLE), REJECT_INVALID, "bad-tx-signature");

    return true;
}

/**################################ Universal Coin Transfer ########################################**/

string SingleTransfer::ToString(const CAccountDBCache &accountCache) const {
    return strprintf("to_uid=%s, coin_symbol=%s, coin_amount=%llu", to_uid.ToDebugString(), coin_symbol, coin_amount);
}

Object SingleTransfer::ToJson(CCacheWrapper &cw) const {
    Object result;

    CKeyID desKeyId;
    cw.accountCache.GetKeyId(to_uid, desKeyId);
    result.push_back(Pair("to_uid",      to_uid.ToString()));
    result.push_back(Pair("to_addr",     desKeyId.ToAddress()));
    result.push_back(Pair("coin_symbol", coin_symbol));
    result.push_back(Pair("coin_amount", coin_amount));

    return result;
}