// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "account.h"
#include "../logging.h"
#include "config/configuration.h"
#include "config/const.h"
#include "main.h"


bool CAccount::GetBalance(const TokenSymbol &tokenSymbol, const BalanceType balanceType, uint64_t &value) {
    const auto iter = tokens.find(tokenSymbol);
    if (iter == tokens.end())
        return false; // token not found

    const auto &accountToken = iter->second;

    switch (balanceType) {
        case FREE_VALUE:    value = accountToken.free_amount;    break;
        case STAKED_VALUE:  value = accountToken.staked_amount;  break;
        case FROZEN_VALUE:  value = accountToken.frozen_amount;  break;
        case VOTED_VALUE:   value = accountToken.voted_amount;   break;
        case PLEDGED_VALUE: value = accountToken.pledged_amount; break;
        default: return false;
    }

    return true;
}

bool CAccount::OperateBalance(const TokenSymbol &tokenSymbol, const BalanceOpType opType, const uint64_t &value,
                            ReceiptType receiptType, ReceiptList &receipts, CAccount *pOtherAccount) {

    CAccountToken &accountToken = tokens[tokenSymbol];
    CUserID fromUid = CUserID(keyid);
    CUserID toUid = CNullID();

    CReceipt receipt(receiptType, opType);

    if (pOtherAccount != nullptr && this != pOtherAccount && this->keyid == pOtherAccount->keyid) {
        return ERRORMSG("account this != pOtherAccount, but the keyid is same, addr=%s",
                    this->keyid.ToAddress());
    }

    switch (opType) {
        case ADD_FREE: {
            accountToken.free_amount += value;

            if (pOtherAccount != nullptr) {
                CAccountToken peerToken = pOtherAccount->GetToken(tokenSymbol);
                if (peerToken.free_amount < value)
                    return ERRORMSG("peer token free_amount insufficient(%llu vs %llu) of %s",
                                peerToken.free_amount, value, tokenSymbol);

                peerToken.free_amount -= value;
                pOtherAccount->SetToken(tokenSymbol, peerToken);
                toUid = CUserID(pOtherAccount->keyid);
            } else {
                toUid = fromUid;
                fromUid = CNullID();
            }

            break;
        }
        case SUB_FREE: {
            if (accountToken.free_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.free_amount -= value;

            if (pOtherAccount != nullptr) {
                CAccountToken peerToken = pOtherAccount->GetToken(tokenSymbol);

                peerToken.free_amount += value;
                pOtherAccount->SetToken(tokenSymbol, peerToken);
                toUid = CUserID(pOtherAccount->keyid);
            }

            break;
        }
        case STAKE: {
            if (accountToken.free_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.free_amount -= value;
            accountToken.staked_amount += value;
            break;
        }
        case UNSTAKE: {
            if (accountToken.staked_amount < value)
                return ERRORMSG("staked_amount insufficient(%llu vs %llu) of %s",
                                accountToken.staked_amount, value, tokenSymbol);

            accountToken.free_amount += value;
            accountToken.staked_amount -= value;
            break;
        }
        case FREEZE: {
            if (accountToken.free_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.free_amount -= value;
            accountToken.frozen_amount += value;
            break;
        }
        case UNFREEZE: {
            if (accountToken.frozen_amount < value)
                return ERRORMSG("frozen_amount insufficient(%llu vs %llu) of %s",
                                accountToken.frozen_amount, value, tokenSymbol);

            accountToken.free_amount += value;
            accountToken.frozen_amount -= value;
            break;
        }
        case VOTE: {
            if (accountToken.free_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.free_amount -= value;
            accountToken.voted_amount += value;
            break;
        }
        case UNVOTE: {
            if (accountToken.voted_amount < value)
                return ERRORMSG("voted_amount insufficient(%llu vs %llu) of %s",
                                accountToken.voted_amount, value, tokenSymbol);

            accountToken.free_amount += value;
            accountToken.voted_amount -= value;
            break;
        }
        case PLEDGE: {
            if (accountToken.free_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.free_amount -= value;
            accountToken.pledged_amount += value;
            break;
        }
        case UNPLEDGE: {
            if (accountToken.pledged_amount < value)
                return ERRORMSG("pledged_amount insufficient(%llu vs %llu) of %s",
                                accountToken.pledged_amount, value, tokenSymbol);

            accountToken.free_amount += value;
            accountToken.pledged_amount -= value;
            break;
        }
        case DEX_DEAL: {
            if (pOtherAccount == nullptr)
                return ERRORMSG("peer account not specified!");

            if (accountToken.frozen_amount < value)
                return ERRORMSG("free_amount insufficient(%llu vs %llu) of %s",
                                accountToken.free_amount, value, tokenSymbol);

            accountToken.frozen_amount -= value; // must operate the subtract first, because maybe this==pOtherAccount

            CAccountToken peerToken = pOtherAccount->GetToken(tokenSymbol);
            peerToken.free_amount += value;
            pOtherAccount->SetToken(tokenSymbol, peerToken);
            toUid = CUserID(pOtherAccount->keyid);
            break;

        }
        default: return false;
    }

    receipt.SetInfo(fromUid, toUid, tokenSymbol, value);
    receipts.push_back(receipt);
    return true;
}

uint64_t CAccount::ComputeVoteBcoinInterest(const uint64_t lastVotedBcoins, const uint32_t currHeight) {
    if (lastVotedBcoins == 0) {
        return 0;  // 0 for the very 1st vote
    }

    uint64_t beginHeight = last_vote_height;
    uint64_t endHeight   = currHeight;
    uint8_t beginSubsidy = ::GetSubsidyRate(last_vote_height);
    uint8_t endSubsidy   = ::GetSubsidyRate(currHeight);

    auto ComputeInterest = [currHeight](const CRegID &regid, const uint64_t votedBcoins, const uint8_t subsidy,
                              const uint64_t beginHeight, const uint64_t endHeight,
                              const uint32_t yearHeight) -> uint64_t {
        uint64_t holdHeight = endHeight - beginHeight;
        uint64_t interest   = (long double)votedBcoins * holdHeight * subsidy / yearHeight / 100;

        LogPrint(BCLog::PROFIT,
                 "compute vote staking interest to voter: %s, current height: %u\n"
                 "interest = votedBcoins * (endHeight - beginHeight) * subsidy / yearHeight / 100\n"
                 "formula: %llu = 1.0 * %llu * (%llu - %llu) * %u / %u / 100\n",
                 regid.ToString(), currHeight, interest, votedBcoins, endHeight, beginHeight, subsidy, yearHeight);
        return interest;
    };

    uint32_t yearHeight = ::GetYearBlockCount(currHeight);
    uint64_t interest   = 0;
    uint8_t subsidy     = beginSubsidy;
    while (subsidy != endSubsidy) {
        uint32_t jumpHeight = ::GetJumpHeightBySubsidy(subsidy - 1);
        interest += ComputeInterest(regid, lastVotedBcoins, subsidy, beginHeight, jumpHeight, yearHeight);
        beginHeight = jumpHeight;
        subsidy -= 1;
    }

    interest += ComputeInterest(regid, lastVotedBcoins, subsidy, beginHeight, endHeight, yearHeight);

    return interest;
}

uint64_t CAccount::ComputeVoteFcoinInterest(const uint64_t lastVotedBcoins, const uint32_t currBlockTime) {
    if (lastVotedBcoins == 0)
        return 0;

    uint32_t epoch_last_vote = last_vote_epoch;
    uint32_t epoch_curr_vote = currBlockTime;

    if (epoch_last_vote >= epoch_curr_vote)
        return 0;

    if (SysCfg().NetworkID() == MAIN_NET) {
        if (epoch_curr_vote <= FCOIN_VOTEMINE_EPOCH_FROM ||
            epoch_last_vote >= FCOIN_VOTEMINE_EPOCH_TO)
            return 0;

        if (epoch_last_vote < FCOIN_VOTEMINE_EPOCH_FROM)
            epoch_last_vote = FCOIN_VOTEMINE_EPOCH_FROM;

        if (epoch_curr_vote > FCOIN_VOTEMINE_EPOCH_TO)
            epoch_curr_vote = FCOIN_VOTEMINE_EPOCH_TO;
    }

    assert(epoch_curr_vote > epoch_last_vote);
    uint32_t duration = epoch_curr_vote - epoch_last_vote;

    // interest = (1% * 21 billion) * (lastVotedBcoins/0.21 billion) * (duration / 365*24*3600)
    uint64_t interest = lastVotedBcoins * (duration / 31536000.0);
    LogPrint(BCLog::PROFIT,
             "compute inflate interest to voter: %s, current time: %u\n"
             "interest = last_voted_bcoins * (epoch_last_vote - epoch_curr_vote) / yearTime\n"
             "formula: %llu = 1.0 * %llu * (%u - %u) / (365 * 24 * 3600)\n",
             regid.ToString(), currBlockTime, interest, lastVotedBcoins, epoch_last_vote, epoch_curr_vote);

    return interest;
}

uint64_t CAccount::ComputeBlockInflateInterest(const uint32_t currHeight, const VoteDelegate &curDelegate, const uint32_t totalDelegateNum) const {

    FeatureForkVersionEnum version = GetFeatureForkVersion(currHeight);
    if (version == MAJOR_VER_R1)
        return 0;

    uint64_t activeVotes = received_votes;
    if (version >= MAJOR_VER_R3) {
        activeVotes = curDelegate.votes;
    }

    uint8_t subsidy      = ::GetSubsidyRate(currHeight);
    uint64_t holdHeight  = 1;
    uint32_t yearHeight  = ::GetYearBlockCount(currHeight);
    uint64_t interest    = (long double)activeVotes * totalDelegateNum * holdHeight * subsidy / yearHeight / 100;

    LogPrint(BCLog::PROFIT,
             "[%d] compute block inflate interest to miner: %s "
             "interest = activeVotes * delegateNum * holdHeight * subsidy / yearHeight / 100\n"
             "formula: %llu = 1.0 * %llu * %u * %llu * %u / %u / 100\n",
             currHeight, regid.ToString(), interest, activeVotes, totalDelegateNum, holdHeight, subsidy, yearHeight);

    return interest;
}

CAccountToken CAccount::GetToken(const TokenSymbol &tokenSymbol) const {
    const auto iter = tokens.find(tokenSymbol);
    if (iter != tokens.end())
        return iter->second;

    return CAccountToken(); //initialize the token
}

bool CAccount::SetToken(const TokenSymbol &tokenSymbol, const CAccountToken &accountToken) {
    tokens[tokenSymbol] = accountToken;
    return true;
}

Object CAccount::ToJsonObj() const {
    vector<CCandidateReceivedVote> candidateVotes;
    pCdMan->pDelegateCache->GetCandidateVotes(regid, candidateVotes);

    Array candidateVoteArray;
    for (auto &vote : candidateVotes) {
        candidateVoteArray.push_back(vote.ToJson());
    }

    Object tokenMapObj;
    for (auto tokenPair : tokens) {
        Object tokenObj;
        const CAccountToken &token = tokenPair.second;

        uint64_t total_amount = token.free_amount + token.staked_amount + token.frozen_amount
                                + token.voted_amount + token.pledged_amount;

        if (total_amount == 0)
            continue;

        tokenObj.push_back(Pair("free_amount",      JsonValueFromAmount(token.free_amount)));    //FREE
        tokenObj.push_back(Pair("staked_amount",    JsonValueFromAmount(token.staked_amount)));  //Stake
        tokenObj.push_back(Pair("frozen_amount",    JsonValueFromAmount(token.frozen_amount)));  //DEX
        tokenObj.push_back(Pair("voted_amount",     JsonValueFromAmount(token.voted_amount)));   //VOTES
        tokenObj.push_back(Pair("pledged_amount",   JsonValueFromAmount(token.pledged_amount))); //CDP
        tokenObj.push_back(Pair("total_amount",     JsonValueFromAmount(total_amount)));         //Total

        tokenMapObj.push_back(Pair(tokenPair.first, tokenObj));
    }

    Object obj;
    string permsString;
    ConvertPermsToString(perms_sum, kAccountPermTitleMap.size(), permsString);

    obj.push_back(Pair("address",           keyid.ToAddress()));
    obj.push_back(Pair("keyid",             keyid.ToString()));
    obj.push_back(Pair("regid",             regid.ToString()));
    obj.push_back(Pair("regid_mature",      regid.IsMature(chainActive.Height())));
    obj.push_back(Pair("owner_pubkey",      owner_pubkey.ToString()));
    obj.push_back(Pair("miner_pubkey",      miner_pubkey.ToString()));
    obj.push_back(Pair("perms",             permsString));
    obj.push_back(Pair("tokens",            tokenMapObj));
    obj.push_back(Pair("received_votes",    JsonValueFromAmount(received_votes)));
    obj.push_back(Pair("vote_list",         candidateVoteArray));

    return obj;
}

string CAccount::ToString() const {
    string str;
    string  strTokens = "";
    for (auto pair : tokens) {
        CAccountToken &token = pair.second;
        strTokens += strprintf ("\n%s:{free=%llu, frozen=%llu, staked=%llu, voted=%llu, pledged=%llu}",
                    pair.first, token.free_amount, token.frozen_amount,
                    token.staked_amount, token.voted_amount,
                    token.pledged_amount);
    }
    str += strprintf(
        "regid=%s, keyid=%s, owner_pubkey=%s, miner_pubkey=%s, "
        "tokens={%s}, received_votes=%llu, last_vote_height=%llu\n",
        regid.ToString(), keyid.GetHex(), owner_pubkey.ToString(), miner_pubkey.ToString(),
        strTokens, received_votes, last_vote_height);

    str += "candidate vote list: \n";

    vector<CCandidateReceivedVote> candidateVotes;
    pCdMan->pDelegateCache->GetCandidateVotes(regid, candidateVotes);
    for (auto & vote : candidateVotes) {
        str += vote.ToString();
    }

    return str;
}

bool CAccount::IsBcoinWithinRange(uint64_t nAddMoney) {
    if (!CheckBaseCoinRange(nAddMoney))
        return ERRORMSG("money:%lld larger than MaxMoney", nAddMoney);

    return true;
}

bool CAccount::IsFcoinWithinRange(uint64_t nAddMoney) {
    if (!CheckFundCoinRange(nAddMoney))
        return ERRORMSG("money:%lld larger than MaxMoney", nAddMoney);

    return true;
}

bool CAccount::ProcessCandidateVotes(const vector<CCandidateVote> &candidateVotesIn,
                                     vector<CCandidateReceivedVote> &candidateVotesInOut, const uint32_t currHeight,
                                     const uint32_t currBlockTime, const CAccountDBCache &accountCache,
                                     vector<CReceipt> &receipts) {
    if (currHeight < last_vote_height) {
        LogPrint(BCLog::ERROR, "currHeight (%u) < last_vote_height (%llu)\n", currHeight, last_vote_height);
        return false;
    }

    auto featureForkVersion = GetFeatureForkVersion(currHeight);
    uint64_t lastTotalVotes = GetToken(SYMB::WICC).voted_amount;

    for (const auto &vote : candidateVotesIn) {
        const CUserID &voteId = vote.GetCandidateUid();
        vector<CCandidateReceivedVote>::iterator itVote =
            find_if(candidateVotesInOut.begin(), candidateVotesInOut.end(),
                    [&voteId, &accountCache](const CCandidateReceivedVote &vote) {
                        if (!voteId.is_same_type(vote.GetCandidateUid())) {
                            CAccount account;
                            if (voteId.is<CRegID>()) {
                                accountCache.GetAccount(voteId, account);
                                return vote.GetCandidateUid() == account.owner_pubkey;

                            } else {  // vote.GetCandidateUid().type() == typeid(CPubKey)
                                accountCache.GetAccount(vote.GetCandidateUid(), account);
                                return account.owner_pubkey == voteId;
                            }
                        } else {
                            return vote.GetCandidateUid() == voteId;
                        }
                    });

        int32_t voteType = VoteType(vote.GetCandidateVoteType());
        if (ADD_BCOIN == voteType) {
            if (itVote != candidateVotesInOut.end()) { //existing vote
                uint64_t currVotes = itVote->GetVotedBcoins();

                if (!IsBcoinWithinRange(vote.GetVotedBcoins()))
                    return ERRORMSG("oper fund value exceeds maximum ");

                itVote->SetVotedBcoins(currVotes + vote.GetVotedBcoins());

                if (!IsBcoinWithinRange(itVote->GetVotedBcoins()))
                    return ERRORMSG("fund value exceeds maximum");

            } else {  // new vote
                if (candidateVotesInOut.size() == IniCfg().GetMaxVoteCandidateNum()) {
                    return ERRORMSG("MaxVoteCandidateNum reached. Must revoke old votes 1st.");
                }

                candidateVotesInOut.push_back(vote);
            }
        } else if (MINUS_BCOIN == voteType) {
            // if (currHeight - last_vote_height < 100) {
            //     return ERRORMSG("last vote not cooled down yet: lastVoteHeigh=%d",
            //                     last_vote_height);
            // }
            if  (itVote != candidateVotesInOut.end()) { //existing vote
                uint64_t currVotes = itVote->GetVotedBcoins();

                if (!IsBcoinWithinRange(vote.GetVotedBcoins()))
                    return ERRORMSG("oper fund value exceeds maximum ");

                if (itVote->GetVotedBcoins() < vote.GetVotedBcoins())
                    return ERRORMSG("oper fund value exceeds delegate fund value");

                itVote->SetVotedBcoins(currVotes - vote.GetVotedBcoins());

                if (0 == itVote->GetVotedBcoins())
                    candidateVotesInOut.erase(itVote);

            } else {
                return ERRORMSG("revocation votes not exist");
            }
        } else {
            return ERRORMSG("operType: %d invalid", voteType);
        }
    }

    // sort account votes after the operations against the new votes
    std::sort(candidateVotesInOut.begin(), candidateVotesInOut.end(),
            [](CCandidateReceivedVote vote1, CCandidateReceivedVote vote2) {
        return vote1.GetVotedBcoins() > vote2.GetVotedBcoins();
    });

    uint64_t newTotalVotes = 0;
    if (!candidateVotesInOut.empty()) {
         if (featureForkVersion >= MAJOR_VER_R2)  {
            for (const auto &vote : candidateVotesInOut) {
                newTotalVotes += vote.GetVotedBcoins();         // one bcoin one vote
            }
        } else { // featureForkVersion == MAJOR_VER_R1
            newTotalVotes = candidateVotesInOut[0].GetVotedBcoins(); // one bcoin eleven votes
        }
    }

    if (newTotalVotes > lastTotalVotes) {
        uint64_t addedVotes = newTotalVotes - lastTotalVotes;
        if (!OperateBalance(SYMB::WICC, BalanceOpType::VOTE, addedVotes, ReceiptType::DELEGATE_ADD_VOTE, receipts)) {
            return ERRORMSG("delegate votes exceeds account bcoins when voting! "
                            "newTotalVotes=%llu, lastTotalVotes=%llu, freeAmount=%llu",
                            newTotalVotes, lastTotalVotes, GetToken(SYMB::WICC).free_amount);
        }

    } else if (newTotalVotes < lastTotalVotes) {
        uint64_t subVotes = lastTotalVotes - newTotalVotes;
        if (!OperateBalance(SYMB::WICC, BalanceOpType::UNVOTE, subVotes, ReceiptType::DELEGATE_SUB_VOTE, receipts)) {
            return ERRORMSG("delegate votes insufficient to unvote! "
                            "newTotalVotes=%llu, lastTotalVotes=%llu, freeAmount=%llu",
                            newTotalVotes, lastTotalVotes, GetToken(SYMB::WICC).free_amount);
        }
    } // else newTotalVotes == lastTotalVotes // do nothing

    // collect inflated bcoins or fcoins
    if (featureForkVersion >= MAJOR_VER_R2)  {
        // only fcoins will be inflated for voters
        uint64_t fcoinAmountToInflate = ComputeVoteFcoinInterest(lastTotalVotes, currBlockTime);

        if (fcoinAmountToInflate > 0) {
            if (!OperateBalance(SYMB::WGRT, BalanceOpType::ADD_FREE, fcoinAmountToInflate,
                                 ReceiptType::DELEGATE_VOTE_INTEREST, receipts)) {
                return ERRORMSG("add fcoins to inflate failed");
            }
        }

        LogPrint(BCLog::PROFIT, "Account(%s) received vote staking interest amount (fcoins): %llu\n", regid.ToString(),
                    fcoinAmountToInflate);
    } else { // featureForkVersion == MAJOR_VER_R1
        // for backward compatibility
        uint64_t bcoinAmountToInflate = ComputeVoteBcoinInterest(lastTotalVotes, currHeight);
        if (!IsBcoinWithinRange(bcoinAmountToInflate))
            return false;

        if (!OperateBalance(SYMB::WICC, BalanceOpType::ADD_FREE, bcoinAmountToInflate,
                            ReceiptType::DELEGATE_VOTE_INTEREST, receipts)) {
            return ERRORMSG("add bcoins to inflate failed");
        }

        LogPrint(BCLog::PROFIT, "Account(%s) received vote staking interest amount (bcoins): %llu\n",
                regid.ToString(), bcoinAmountToInflate);
    }

    // Attention: update last vote height/last vote epoch after computing vote staking interest.
    last_vote_height = currHeight;
    last_vote_epoch  = currBlockTime;

    return true;
}

bool CAccount::StakeVoteBcoins(VoteType type, const uint64_t votes) {
    switch (type) {
        case ADD_BCOIN: {
            received_votes += votes;
            if (!IsBcoinWithinRange(received_votes)) {
                return ERRORMSG("StakeVoteBcoins() : delegates total votes exceed maximum ");
            }

            break;
        }

        case MINUS_BCOIN: {
            if (received_votes < votes) {
                return ERRORMSG("StakeVoteBcoins() : delegates total votes less than revocation vote number");
            }
            received_votes -= votes;

            break;
        }

        default:
            return ERRORMSG("StakeVoteBcoins() : CDelegateVoteTx ExecuteTx AccountVoteOper revocation votes do not exist");
    }

    return true;
}

bool CAccount::IsSelfUid(const CUserID &uid) {
    if (uid.is<CKeyID>()) {
        return keyid == uid.get<CKeyID>();
    } else if (uid.is<CRegID>()) {
        return !regid.IsEmpty() && regid == uid.get<CRegID>();
    } else if (uid.is<CPubKey>()) {
        return owner_pubkey.IsValid() && owner_pubkey == uid.get<CPubKey>();
    }

    return false;
}

bool CAccount::CheckBalance(const TokenSymbol& symbol, const BalanceType& balanceType, const uint64_t& value) {
    uint64_t balance;
    auto iter = tokens.find(symbol);
    if (iter == tokens.end())
        balance = 0; // token not found
    else {
        switch (balanceType) {
            case FREE_VALUE:
                balance = iter->second.free_amount;
                break;
            case STAKED_VALUE:
                balance = iter->second.staked_amount;
                break;
            case FROZEN_VALUE:
                balance = iter->second.frozen_amount;
                break;
            case VOTED_VALUE:
                balance = iter->second.voted_amount;
                break;
            case PLEDGED_VALUE:
                balance = iter->second.pledged_amount;
                break;
            case NULL_TYPE:
            default:
                throw runtime_error("balance type error");
        }
    }

    return balance >= value;

}

///////////////////////////////////////////////////////////////////////////////
// class CVmOperate

Object CVmOperate::ToJson() {
    Object obj;
    if (accountType == AccountType::REGID) {
        vector<uint8_t> vRegId(accountId, accountId + 6);
        CRegID regId(vRegId);
        obj.push_back(Pair("regid", regId.ToString()));
    } else if (accountType == AccountType::BASE58ADDR) {
        string addr(accountId, accountId + sizeof(accountId));
        obj.push_back(Pair("addr", addr));
    }
    obj.push_back(Pair("opertype", GetBalanceOpTypeName((BalanceOpType)opType)));
    if (timeoutHeight > 0)
        obj.push_back(Pair("outHeight", (int32_t)timeoutHeight));

    uint64_t amount;
    memcpy(&amount, money, sizeof(money));
    obj.push_back(Pair("amount", amount));
    return obj;
}
