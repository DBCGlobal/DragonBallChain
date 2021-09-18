// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "assetdb.h"

#include "commons/uint256.h"
#include "commons/util/util.h"

#include <stdint.h>

using namespace std;

bool CAssetDbCache::GetAsset(const TokenSymbol &tokenSymbol, CAsset &asset) {
    return asset_cache.GetData(tokenSymbol, asset);
}

bool CAssetDbCache::SetAsset(const CAsset &asset) {
    return asset_cache.SetData(asset.asset_symbol, asset);
}

bool CAssetDbCache::HasAsset(const TokenSymbol &tokenSymbol) {
    return asset_cache.HasData(tokenSymbol);
}

bool CAssetDbCache::CheckAsset(const TokenSymbol &symbol, CAsset &asset) {
    if (symbol.size() < MIN_TOKEN_SYMBOL_LEN || symbol.size() > MAX_TOKEN_SYMBOL_LEN) {
        LogPrint(BCLog::INFO, "[WARN] Invalid symobl format: %s\n", symbol);
        return false;
    }

    if (!GetAsset(symbol, asset)) {
        LogPrint(BCLog::INFO, "[WARN] Asset(%s) does not exist\n", symbol);
        return false;
    }

    return true;
}

bool CAssetDbCache::CheckAsset(const TokenSymbol &symbol, const uint64_t permsSum) {
    CAsset asset;
    if (!CheckAsset(symbol, asset))
        return false;

    if (permsSum == 0)
        return true;

    LogPrint(BCLog::DEBUG, "Asset(%s) to check perms: %llu vs actual %llu\n", symbol, permsSum, asset.perms_sum);

    return asset.HasPerms(permsSum);
}

bool CAssetDbCache::GetAssetTokensByPerm(const AssetPermType& permType, set<TokenSymbol> &symbolSet) {

    CDbIterator<DbAssetCache> it(asset_cache);
   // CPermAssetsIterator it(perm_assets_cache, CFixedUInt64(permType));
    for (it.First(); it.IsValid(); it.Next()) {
        if (it.GetValue().HasPerms(permType))
            symbolSet.insert(it.GetKey());
    }

    return true;
}

bool CAssetDbCache::CheckPriceFeedQuoteSymbol(const TokenSymbol &quoteSymbol) {
    if (kPriceQuoteSymbolSet.count(quoteSymbol) == 0)
        return ERRORMSG("unsupported price quote_symbol=%s", quoteSymbol);

    return true;
}

bool CAssetDbCache::CheckDexBaseSymbol(const TokenSymbol &baseSymbol) {
    CAsset baseAsset;
    if (!GetAsset(baseSymbol, baseAsset))
        return ERRORMSG("dex base_symbol=%s not exist", baseSymbol);

    if (!baseAsset.HasPerms(AssetPermType::PERM_DEX_BASE))
        return ERRORMSG("dex base_symbol=%s has no PERM_DEX_BASE", baseSymbol);

    return true;
}

bool CAssetDbCache::CheckDexQuoteSymbol(const TokenSymbol &quoteSymbol) {

    CAsset quoteAsset;
    if (!GetAsset(quoteSymbol, quoteAsset))
        return ERRORMSG("dex quote_symbol=%s not exist", quoteSymbol);

    if (!quoteAsset.HasPerms(AssetPermType::PERM_DEX_QUOTE))
        return ERRORMSG("dex quote_symbol=%s not have PERM_DEX_BASE", quoteSymbol);

    return true;
}

bool CAssetDbCache::SetAxcSwapPair(const CAxcSwapPairStore &value) {
    return axc_swap_coin_ps_cache.SetData(value.peer_symbol, value) &&
           axc_swap_coin_sp_cache.SetData(value.GetSelfSymbol(), value);
}

bool CAssetDbCache::HasAxcCoinPairByPeerSymbol(TokenSymbol peerSymbol) {
    return axc_swap_coin_ps_cache.HasData(peerSymbol);
}

bool CAssetDbCache::GetAxcCoinPairBySelfSymbol(TokenSymbol selfSymbol, CAxcSwapPairStore& swapPair) {
    return axc_swap_coin_sp_cache.GetData(selfSymbol, swapPair);
}

bool CAssetDbCache::GetAxcCoinPairByPeerSymbol(TokenSymbol peerSymbol, CAxcSwapPairStore& swapPair) {
    return axc_swap_coin_ps_cache.GetData(peerSymbol, swapPair);
}
