// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#ifndef MINER_PBFTMANAGER_H
#define MINER_PBFTMANAGER_H

#include "chain/chain.h"

class CBlockConfirmMessage;
class CBlockFinalityMessage;
class CPBFTMessage;

class CPBFTMan {

private:
    CBlockIndex* localFinIndex = nullptr;
    int64_t localFinLastUpdate = 0;
    CBlockIndex* globalFinIndex = nullptr;
    uint256 globalFinHash = uint256();
    CCriticalSection cs_finblock;
    bool UpdateLocalFinBlock(const uint32_t height);
    bool UpdateGlobalFinBlock(const uint32_t height);

public:

    CBlockIndex *GetLocalFinIndex();
    CBlockIndex *GetGlobalFinIndex();
    uint256 GetGlobalFinBlockHash();
//    bool UpdateFinalityBlock();
    bool SetLocalFinTimeout();
    bool UpdateLocalFinBlock(const CBlockIndex* pIndex);
    bool UpdateLocalFinBlock(const CBlockConfirmMessage& msg, const uint32_t messageCount);
    bool UpdateGlobalFinBlock(const CBlockIndex* pIndex);
    bool UpdateGlobalFinBlock(const CBlockFinalityMessage& msg, const uint32_t messageCount);
    int64_t  GetLocalFinLastUpdate() const;
};

bool BroadcastBlockConfirm(const CBlockIndex* block);

bool BroadcastBlockFinality(const CBlockIndex* block);

bool CheckPBFTMessage(const int32_t msgType ,const CPBFTMessage& msg);

bool CheckPBFTMessageSigner(const CPBFTMessage& msg);
bool RelayBlockConfirmMessage(const CBlockConfirmMessage& msg);

bool RelayBlockFinalityMessage(const CBlockFinalityMessage& msg);
#endif //MINER_PBFTMANAGER_H
