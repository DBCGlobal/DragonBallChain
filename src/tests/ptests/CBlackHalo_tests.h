// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CBLACKHALO_TESTS_H_
#define CBLACKHALO_TESTS_H_

#include "CycleTestBase.h"
#include <stdlib.h>
#include <time.h>
#include "main.h"
#include "miner/miner.h"
#include "commons/uint256.h"
#include "commons/util/util.h"
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

#include "commons/json/json_spirit_writer_template.h"
#include "./rpc/core/rpcclient.h"
#include "commons/json/json_spirit_reader_template.h"
#include "commons/json/json_spirit_reader.h"
#include "commons/json/json_spirit_writer.h"
#include "commons/json/json_spirit_value.h"
#include "commons/json/json_spirit_stream_reader.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
using namespace std;
using namespace boost;
using namespace json_spirit;


typedef struct  {
	unsigned char dnType;					//!<����
	unsigned char seller[6];			    //!<����ID������6�ֽڵ��˻�ID��
	IMPLEMENT_SERIALIZE
	(
			READWRITE(dnType);
			for(int i = 0;i < 6;i++)
			READWRITE(seller[i]);
	)
} FIRST_CONTRACT;

typedef struct {
	unsigned char dnType;				//!<��������
	unsigned char hash[32];		        //!<��һ�����װ��Ĺ�ϣ
	IMPLEMENT_SERIALIZE
	(
		READWRITE(dnType);
		for(int i = 0;i < 32;i++)
		READWRITE(hash[i]);
	)
} NEXT_CONTRACT;

enum TXTYPE{
	TX_BUYTRADE = 0x01,
	TX_SELLERTRADE = 0x02,
	TX_BUYERCONFIM = 0x03,
	TX_BUYERCANCEL = 0x04,
};
#define BUYER_A    "dk2NNjraSvquD9b4SQbysVRQeFikA55HLi"
#define SELLER_B    "dggsWmQ7jH46dgtA5dEZ9bhFSAK1LASALw"


class CBlackHalo: public CycleTestBase {
  int step;
	string sritpthash;
	string buyerhash;
	string sellerhash;
	string buyerconfiredhash;
	string buyercancelhash;
	string scriptid ;
	uint64_t sendmonye;
public:
	CBlackHalo();
	virtual ~CBlackHalo();
	int GetRandomFee() {
		srand(time(NULL));
		int r = (rand() % 1000000) + 100000000;
		return r;
	}
	virtual TEST_STATE Run() ;
	uint64_t GetPayMoney() {
		uint64_t r = 0;
		while(true)
		{
			srand(time(NULL));
			r = (rand() % 1000002) + 100000000;
			if(r%2 == 0 && r != 0)
				break;
		}

		return r;
	}
	bool RegistScript();
	bool SendBuyerPackage();
	bool SendSellerPackage();
	bool SendBuyerConfirmedPackage();
	bool SendBuyerCancelPackage();
	bool WaitRegistScript();
	bool WaitSendBuyerPackage();
	bool WaitSendSellerPackage();
	bool WaitSendBuyerConfirmedPackage();
	bool WaitSendBuyerCancelPackage();
};

#endif /* CBLACKHALO_TESTS_H_ */
