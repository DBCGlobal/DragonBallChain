// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "pbftcontext.h"
#include "p2p/protocol.h"
#include "logging.h"

CPBFTContext pbftContext ;

bool CPBFTContext::GetMinerListByBlockHash(const uint256 blockHash, set<CRegID>& miners) {

    LOCK(cs_block_miner_list);
    auto it = blockMinerListMap.find(blockHash);
    if (it == blockMinerListMap.end())
        return false;

    miners = it->second;

    return true;
}

bool CPBFTContext::SaveMinersByHash(uint256 blockhash, VoteDelegateVector delegates) {
    set<CRegID> miners;
    for (auto &delegate : delegates)
        miners.insert(delegate.regid);

    LOCK(cs_block_miner_list);
    blockMinerListMap.insert(std::make_pair(blockhash, miners));

    return true;
}