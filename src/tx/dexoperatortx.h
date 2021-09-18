// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TX_DEX_OPERATOR_TX_H
#define TX_DEX_OPERATOR_TX_H

#include "entities/asset.h"
#include "entities/dexorder.h"
#include "tx.h"
#include "persistence/dexdb.h"

typedef vector<CVarIntValue<uint64_t>> DexOpIdValueList;

class CDEXOperatorRegisterTx: public CBaseTx {
public:
    struct Data {
        CUserID owner_uid;                   // owner uid of exchange
        CUserID fee_receiver_uid;            // fee receiver uid
        string name              = "";       // domain name
        string portal_url        = "";       // portal url of dex operator
        dex::OpenMode order_open_mode   = dex::OpenMode::PRIVATE; // the default public mode for creating order
        uint64_t maker_fee_ratio = 0;    // the default maker fee ratio for creating order
        uint64_t taker_fee_ratio = 0;    // the defalt taker fee ratio for creating order
        DexOpIdValueList order_open_dexop_list;
        string memo              = "";

        IMPLEMENT_SERIALIZE(
            READWRITE(owner_uid);
            READWRITE(fee_receiver_uid);
            READWRITE(name);
            READWRITE(portal_url);
            READWRITE((uint8_t&)order_open_mode);
            READWRITE(VARINT(maker_fee_ratio));
            READWRITE(VARINT(taker_fee_ratio));
            READWRITE(order_open_dexop_list);
            READWRITE(memo);
        )
        string ToString() {
            return strprintf("owner_id=%s, fee_receiver_uid =%s, name=%s, portal_url=%s, "
                             "order_open_mode=%d, makefee=%d, takefee=%d, memo=%s, order_open_dexop_list=%s",
                             owner_uid.ToString(), fee_receiver_uid.ToString(), name, portal_url,
                             (uint8_t &)order_open_mode, maker_fee_ratio, taker_fee_ratio, memo,
                             db_util::ToString(order_open_dexop_list));
        }
    };
public:
    Data data;
public:
    CDEXOperatorRegisterTx() : CBaseTx(DEX_OPERATOR_REGISTER_TX) {};

    CDEXOperatorRegisterTx(const CUserID &txUidIn, int32_t validHeightIn,
                           const TokenSymbol &feeSymbol, uint64_t fees,
                           const Data &dataIn)
        : CBaseTx(DEX_OPERATOR_REGISTER_TX, txUidIn, validHeightIn, feeSymbol, fees), data(dataIn) {
    }

    IMPLEMENT_SERIALIZE(
        READWRITE(VARINT(this->nVersion));
        nVersion = this->nVersion;
        READWRITE(VARINT(valid_height));
        READWRITE(txUid);
        READWRITE(fee_symbol);
        READWRITE(VARINT(llFees));

        READWRITE(data);
        READWRITE(signature);
    )

    virtual void SerializeForHash(CHashWriter &hw) const {
        hw << VARINT(nVersion) << uint8_t(nTxType) << VARINT(valid_height) << txUid << fee_symbol
           << VARINT(llFees) << data;
    }

    virtual std::shared_ptr<CBaseTx> GetNewInstance() const { return std::make_shared<CDEXOperatorRegisterTx>(*this); }

    virtual string ToString(CAccountDBCache &accountCache) {
        string baseString = CBaseTx::ToString(accountCache);
        return baseString + data.ToString();
    }
    virtual Object ToJson(CCacheWrapper &cw) const;

    virtual bool CheckTx(CTxExecuteContext &context);
    virtual bool ExecuteTx(CTxExecuteContext &context);
};


class CDEXOperatorUpdateData{

public:
    class CNullDexData {
    public:
        friend bool operator==(const CNullDexData &a, const CNullDexData &b) { return true; }
        friend bool operator<(const CNullDexData &a, const CNullDexData &b) { return true; }
    };
    enum UpdateField: uint8_t{
        UPDATE_NONE             = 0 ,
        OWNER_UID               = 1 ,
        FEE_RECEIVER_UID        = 2 ,
        NAME                    = 3 ,
        PORTAL_URL              = 4 ,
        OPEN_MODE               = 5 ,
        MAKER_FEE_RATIO         = 6 ,
        TAKER_FEE_RATIO         = 7 ,
        ORDER_OPEN_DEXOP_LIST   = 8 ,
        MEMO                    = 9
    };

    typedef boost::variant<CNullDexData,
            CUserID, // receiver_uid,owner_uid
            string,  // name,portal_url,memo
            uint64_t, // taker&maker fee ratio
            dex::OpenMode,
            DexOpIdValueList
    > ValueType;

public:
    uint32_t dexId;
    UpdateField field = UPDATE_NONE;
    ValueType value;

    inline unsigned int GetSerializeSize(int serializedType, int nVersion) const {
        unsigned int baseSize = ::GetSerializeSize(VARINT(dexId),serializedType, nVersion) + sizeof(uint8_t);
        switch (field) {
            case OWNER_UID:
            case FEE_RECEIVER_UID:
                return baseSize + get<CUserID>().GetSerializeSize(serializedType, nVersion);
            case NAME:
            case PORTAL_URL:
            case MEMO:
                return baseSize + ::GetSerializeSize(get<string>(), serializedType, nVersion);
            case OPEN_MODE:
                return baseSize + ::GetSerializeSize((uint8_t)(get<dex::OpenMode>()), serializedType, nVersion);
            case MAKER_FEE_RATIO:
            case TAKER_FEE_RATIO:
                return baseSize + ::GetSerializeSize(VARINT(get<uint64_t>()), serializedType, nVersion);
            case ORDER_OPEN_DEXOP_LIST:
                return baseSize + ::GetSerializeSize(get<DexOpIdValueList>(), serializedType, nVersion);
            default: break;
        }
        return 0;
    }

    template <typename Stream>
    void Serialize(Stream &s, int serializedType, int nVersion) const {
        s << VARINT(dexId);
        s << (uint8_t)field;
        switch (field) {
            case OWNER_UID:
            case FEE_RECEIVER_UID:
                s << get<CUserID>();
                break;
            case NAME:
            case PORTAL_URL:
            case MEMO:
                s << get<string>();
                break;
            case OPEN_MODE:
                s << (uint8_t)get<dex::OpenMode>();
                break;
            case MAKER_FEE_RATIO:
            case TAKER_FEE_RATIO:
                s << VARINT(get<uint64_t>());
                break;
            case ORDER_OPEN_DEXOP_LIST:
                s << get<DexOpIdValueList>();
                break;
            default: {
                LogPrint(BCLog::ERROR, "CDEXOperatorUpdateData::Serialize(), Invalid Asset update field=%d\n", field);
                throw ios_base::failure("Invalid dexoperator update field");
            }
        }
    }

    template <typename Stream>
    void Unserialize(Stream &s, int serializedType, int nVersion) {
        int32_t dexValue;
        s >> VARINT(dexValue);
        dexId = dexValue;

        s >> ((uint8_t&)field);
        switch (field) {
            case FEE_RECEIVER_UID:
            case OWNER_UID: {
                CUserID uidV;
                s >> uidV;
                value = uidV;
                break;
            }
            case PORTAL_URL:
            case MEMO:
            case NAME: {
                string stringV;
                s >> stringV;
                value = stringV;
                break;
            }
            case OPEN_MODE: {
                dex::OpenMode openMode;
                s >> (uint8_t&)openMode;
                value = openMode;
                break;
            }
            case MAKER_FEE_RATIO:
            case TAKER_FEE_RATIO: {
                uint64_t uint64V;
                s >> VARINT(uint64V);
                value = uint64V;
                break;
            }
            case ORDER_OPEN_DEXOP_LIST: {
                DexOpIdValueList orderOpenDexopList;
                s >> orderOpenDexopList;
                value = orderOpenDexopList;
                break;
            }
            default: {
                LogPrint(BCLog::ERROR, "CDexOperatorUpdateData::Unserialize(), Invalid dexoperator update field=%d\n", field);
                throw ios_base::failure("Invalid DexOperator update type");
            }
        }
    }

    template <typename T_Value>
    T_Value &get() {
        return boost::get<T_Value>(value);
    }
    template <typename T_Value>
    const T_Value &get() const {
        return boost::get<T_Value>(value);
    }
    bool Check(CBaseTx &tx, CCacheWrapper &cw, string& errmsg, string& errcode, uint32_t currentHeight);

    bool UpdateToDexOperator(DexOperatorDetail& detail,CCacheWrapper& cw);

    string ValueToString() const {
        switch (field){
            case FEE_RECEIVER_UID:
            case OWNER_UID:
                return get<CUserID>().ToString();
            case NAME:
            case PORTAL_URL:
            case MEMO:
                return get<string>();
            case OPEN_MODE:
                return dex::kOpenModeHelper.GetName(get<dex::OpenMode>());
            case MAKER_FEE_RATIO:
            case TAKER_FEE_RATIO:
                return db_util::ToString(get<uint64_t>());
            case ORDER_OPEN_DEXOP_LIST:
                return db_util::ToString(get<DexOpIdValueList>());
            default:
                return EMPTY_STRING;

        }

    }

};

class CDEXOperatorUpdateTx: public CBaseTx {

public:
    CDEXOperatorUpdateData update_data;
public:
    CDEXOperatorUpdateTx() : CBaseTx(DEX_OPERATOR_UPDATE_TX) {}

    CDEXOperatorUpdateTx(const CUserID &txUidIn, int32_t validHeightIn, const TokenSymbol &feeSymbolIn,
                   uint64_t feesIn, const CDEXOperatorUpdateData &updateData)
            : CBaseTx(DEX_OPERATOR_UPDATE_TX, txUidIn, validHeightIn, feeSymbolIn, feesIn),
            update_data(updateData) {}

    ~CDEXOperatorUpdateTx() {}

    IMPLEMENT_SERIALIZE(
            READWRITE(VARINT(this->nVersion));
            nVersion = this->nVersion;
            READWRITE(VARINT(valid_height));
            READWRITE(txUid);
            READWRITE(fee_symbol);
            READWRITE(VARINT(llFees));
            READWRITE(update_data);
            READWRITE(signature);
    )

    virtual void SerializeForHash(CHashWriter &hw) const {
        hw << VARINT(nVersion) << uint8_t(nTxType) << VARINT(valid_height) << txUid
                   << fee_symbol << VARINT(llFees) << update_data;
    }

    virtual std::shared_ptr<CBaseTx> GetNewInstance() const { return std::make_shared<CDEXOperatorUpdateTx>(*this); }

    virtual string ToString(CAccountDBCache &accountCache);
    virtual Object ToJson(CCacheWrapper &cw) const;

    virtual bool CheckTx(CTxExecuteContext &context);
    virtual bool ExecuteTx(CTxExecuteContext &context);
};




#endif  // TX_DEX_OPERATOR_TX_H