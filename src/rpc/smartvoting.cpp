// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018 The SmartCash developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "core_io.h"
#include "coincontrol.h"
#include "consensus/validation.h"
#include "init.h"
#include "validation.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "smartnode/smartnodesync.h"
#include "smartmining/miningpayments.h"
#include "smartvoting/proposal.h"
#include "smartvoting/manager.h"
#include "smartvoting/votekeys.h"
#include "util.h"
#include "wallet/wallet.h"
#include <univalue.h>


extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry);

UniValue smartvoting(const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    std::vector<std::string> vecCommands = {
#ifdef ENABLE_WALLET
        "prepare",
#endif // ENABLE_WALLET
        "submit",
        "count",
        "deserialize",
        "get",
        "getvotes",
        "getcurrentvotes",
        "list",
        "diff",
        "check"
    };

    if (fHelp  || std::find(vecCommands.begin(), vecCommands.end(), strCommand) == vecCommands.end() )
        throw std::runtime_error(
                "smartvoting \"command\"...\n"
                "Use SmartProposal commands.\n"
                "\nAvailable commands:\n"
                "  check              - Validate a proposal\n"
                "  prepare            - Create and prepare a proposal by signing and creating the fee tx\n"
                "  submit             - Submit a proposal to the network\n"
                "  count              - Count proposals.\n"
                "  list               - List all proposals.\n"
                "  get                - Get a proposal by its hash\n"
                "  getvotes           - Get all votes for a proposal\n"
                );

    // VALIDATE A GOVERNANCE OBJECT PRIOR TO SUBMISSION
    if(strCommand == "check")
    {
//        if (params.size() != 2) {
//            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting check <data-hex>'");
//        }

//        // ASSEMBLE NEW GOVERNANCE OBJECT FROM USER PARAMETERS

//        uint256 hashParent;

//        int nRevision = 1;

//        int64_t nTime = GetAdjustedTime();
//        std::string strDataHex = params[1].get_str();

//        CProposal proposal(hashParent, nRevision, nTime, uint256(), strDataHex);

//        if(proposal.GetObjectType() == SMARTVOTING_OBJECT_PROPOSAL) {
//            CProposalValidator validator(strDataHex);
//            if(!validator.Validate())  {
//                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid proposal data, error messages:" + validator.GetErrorMessages());
//            }
//        }
//        else {
//            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid object type, only proposals can be validated");
//        }

//        UniValue objResult(UniValue::VOBJ);

//        objResult.push_back(Pair("Proposal status", "OK"));

//        return objResult;
    }


#ifdef ENABLE_WALLET
    // PREPARE THE PROPOSAL BY CREATING A COLLATERAL TRANSACTION
    if(strCommand == "prepare")
    {
        if( !pwalletMain )
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not available.");

        if (params.size() != 5) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting prepare <title> <url> <address> <milestone-array>'");
        }

        int64_t nCreationTime = GetAdjustedTime();

        CInternalProposal proposal;

        proposal.SetTitle(params[1].get_str());
        proposal.SetUrl(params[2].get_str());
        proposal.SetAddress(CSmartAddress(params[3].get_str()));

        for( UniValue milestone : params[4].get_array().getValues() ){

            if( !milestone.isObject() ||
                !milestone.exists("timestamp") || !milestone["timestamp"].isNum() ||
                !milestone.exists("amount") || !milestone["amount"].isNum() ||
                !milestone.exists("description") || !milestone["description"].isStr() ){
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct milestones format is: [{\"timestamp\" : <unix timestamp>, \"amount\" : <amount USD>, \"description\" : <description>},{...},..]");
            }

            CProposalMilestone m(milestone["timestamp"].get_int64(), milestone["amount"].get_int64(), milestone["description"].get_str() );

            proposal.AddMilestone(m);
        }

        std::vector<std::string> vecErrors;

        proposal.SetCreationTime(nCreationTime);

        if( !proposal.IsValid(vecErrors) ){

            std::string strError;

            for( auto error : vecErrors ){
                strError += error + "\n";
            }

            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid proposal data, error messages: " + strError);
        }

        LOCK2(cs_main, pwalletMain->cs_wallet);

        EnsureWalletIsUnlocked();

        CWalletTx wtx;
        if(!pwalletMain->GetProposalFeeTX(wtx, proposal.GetAddress(), proposal.GetHash(), SMARTVOTING_PROPOSAL_FEE)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to create the proposal transaction. Please check the balance of the provided proposal address.");
        }

        CReserveKey reservekey(pwalletMain);
        if (!pwalletMain->CommitTransaction(wtx, reservekey, g_connman.get(), NetMsgType::TX)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to send the proposal transaction to the network! Check your connection.");
        }

        // Create the signature of the proposal hash as proof of ownership for
        // the voting portal.

        CKeyID keyID;
        if (!proposal.GetAddress().GetKeyID(keyID)){
            throw JSONRPCError(RPC_INTERNAL_ERROR,"The selected proposal address doesn't refer to a key.");
        }

        CKey key;
        if (!pwalletMain->GetKey(keyID, key)){
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Private key for the proposal address is not available.");
        }

        CDataStream ss(SER_GETHASH, 0);
        ss << strMessageMagic;
        ss << proposal.GetHash().ToString();

        std::vector<unsigned char> vchSig;
        if (!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig)){
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Message signing failed.");
        }

        // Store the signed hash for register in the voting portal
        proposal.SetSignedHash(EncodeBase64(&vchSig[0], vchSig.size()));

        // Set the created tx as proposal fee tx
        proposal.SetFeeHash(wtx.GetHash());
        proposal.SetRawFeeTx(EncodeHexTx(wtx));

        CDataStream ssProposal(SER_NETWORK, PROTOCOL_VERSION);
        ssProposal << static_cast<CProposal>(proposal);

        DBG( std::cout << "smartvoting: prepare "
             << " GetDataAsPlainString = " << proposal.GetDataAsPlainString()
             << ", hash = " << proposal.GetHash().GetHex()
             << ", txidFee = " << wtx.GetHash().GetHex()
             << std::endl; );

        UniValue obj(UniValue::VOBJ);

        obj.pushKV("feeTxHash", wtx.GetHash().ToString());
        obj.pushKV("proposalHash", proposal.GetHash().ToString());
        obj.pushKV("signedHash", proposal.GetSignedHash());
        obj.pushKV("rawProposal", HexStr(ssProposal.begin(), ssProposal.end()));

        return obj;
    }
#endif // ENABLE_WALLET

    /*GetBudgetSystemCollateralTX
        ------ Example Governance Item ------

        smartvoting submit 6e622bb41bad1fb18e7f23ae96770aeb33129e18bd9efe790522488e580a0a03 0 1 1464292854 "beer-reimbursement" 5b5b22636f6e7472616374222c207b2270726f6a6563745f6e616d65223a20225c22626565722d7265696d62757273656d656e745c22222c20227061796d656e745f61646472657373223a20225c225879324c4b4a4a64655178657948726e34744744514238626a6876464564615576375c22222c2022656e645f64617465223a202231343936333030343030222c20226465736372697074696f6e5f75726c223a20225c227777772e646173687768616c652e6f72672f702f626565722d7265696d62757273656d656e745c22222c2022636f6e74726163745f75726c223a20225c22626565722d7265696d62757273656d656e742e636f6d2f3030312e7064665c22222c20227061796d656e745f616d6f756e74223a20223233342e323334323232222c2022676f7665726e616e63655f6f626a6563745f6964223a2037342c202273746172745f64617465223a202231343833323534303030227d5d5d1
    */


    // AFTER COLLATERAL TRANSACTION HAS MATURED USER CAN SUBMIT GOVERNANCE OBJECT TO PROPAGATE NETWORK
    if(strCommand == "submit")
    {
        if (params.size() != 2)  {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting submit <raw-proposal>'");
        }

        if(!smartnodeSync.IsSynced()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Must wait for client to sync with smartnode network. Try again in a few minutes.");
        }

        std::string strRawProposal = params[1].get_str();

        if (!IsHex(strRawProposal))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid proposal data. Must be hex-string");

        vector<unsigned char> rawData(ParseHex(strRawProposal));

        CDataStream ssProposal( rawData ,SER_NETWORK, PROTOCOL_VERSION);

        CProposal proposal;

        ssProposal >> proposal;

        // GET THE PARAMETERS FROM USER

        DBG( std::cout << "smartvoting: submit "
             << " GetDataAsPlainString = " << proposal.GetDataAsPlainString()
             << ", hash = " << proposal.GetHash().ToString()
             << ", txidFee = " << proposal.GetFeeHash().ToString()
             << std::endl; );

        std::string strHash = proposal.GetHash().ToString();

        std::string strError = "";
        int fMissingConfirmations;
        {
            LOCK(cs_main);

            bool fIsValid = proposal.IsValidLocally(strError, fMissingConfirmations, true);
            if(!fIsValid){
                LogPrintf("smartvoting(submit) -- Proposal submission rejected because proposal is not valid - hash = %s, strError = %s\n", strHash, strError);
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Proposal is not valid - " + strHash + " - " + strError);
            }
        }

        LogPrintf("smartvoting(submit) -- Adding locally created proposal - %s\n", strHash);

        if(fMissingConfirmations > 0) {
            smartVoting.AddPostponedProposal(proposal);
            proposal.Relay(*g_connman);
        } else {
            smartVoting.AddProposal(proposal, *g_connman);
        }

        UniValue obj(UniValue::VOBJ);

        obj.pushKV("status", fMissingConfirmations > 0 ? strError : "OK");
        obj.pushKV("proposalHash", proposal.GetHash().ToString());

        return obj;
    }

    if(strCommand == "count") {
        std::string strMode{"json"};

        if (params.size() == 2) {
            strMode = params[1].get_str();
        }

        if (params.size() > 2 || (strMode != "json" && strMode != "all")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting count ( \"json\"|\"all\" )'");
        }

        return strMode == "json" ? smartVoting.ToJson() : smartVoting.ToString();
    }

    // USERS CAN QUERY THE SYSTEM FOR A LIST OF PROPOSALS
    if(strCommand == "list")
    {
        if (params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting [list|diff] ( signal type )'");

        // GET MAIN PARAMETER FOR THIS MODE, VALID OR ALL?

        std::string strCachedSignal = "valid";
        if (params.size() >= 2) strCachedSignal = params[1].get_str();
        if (strCachedSignal != "valid" && strCachedSignal != "funding" && strCachedSignal != "delete" && strCachedSignal != "endorsed" && strCachedSignal != "all")
            return "Invalid signal, should be 'valid', 'funding', 'delete', 'endorsed' or 'all'";

        std::string strType = "all";
        if (params.size() == 3) strType = params[2].get_str();
        if (strType != "proposals" && strType != "triggers" && strType != "all")
            return "Invalid type, should be 'proposals', 'triggers' or 'all'";

        // GET STARTING TIME TO QUERY SYSTEM WITH

        int nStartTime = 0; //list
        if(strCommand == "diff") nStartTime = smartVoting.GetLastDiffTime();

        // SETUP BLOCK INDEX VARIABLE / RESULTS VARIABLE

        UniValue objResult(UniValue::VOBJ);

        // GET MATCHING GOVERNANCE OBJECTS

        LOCK2(cs_main, smartVoting.cs);

        std::vector<const CProposal*> objs = smartVoting.GetAllNewerThan(nStartTime);
        smartVoting.UpdateLastDiffTime(GetTime());

        // CREATE RESULTS FOR USER

        for (const auto& pProposal : objs)
        {
            if(strCachedSignal == "valid" && !pProposal->IsSetCachedValid()) continue;
            if(strCachedSignal == "funding" && !pProposal->IsSetCachedFunding()) continue;
            if(strCachedSignal == "delete" && !pProposal->IsSetCachedDelete()) continue;
            if(strCachedSignal == "endorsed" && !pProposal->IsSetCachedEndorsed()) continue;

            UniValue bObj(UniValue::VOBJ);
            bObj.push_back(Pair("Hash",  pProposal->GetHash().ToString()));
            bObj.push_back(Pair("FeeHash",  pProposal->GetFeeHash().ToString()));
            bObj.push_back(Pair("CreationTime", pProposal->GetCreationTime()));
            const CSmartAddress& proposalAddress = pProposal->GetAddress();
            if(proposalAddress.IsValid()) {
                bObj.push_back(Pair("ProposalAddress", proposalAddress.ToString()));
            }else{
                bObj.push_back(Pair("ProposalAddress", "Invalid"));
            }

            // REPORT STATUS FOR FUNDING VOTES SPECIFICALLY
            bObj.push_back(Pair("AbsoluteYesCount",  pProposal->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("YesCount",  pProposal->GetYesCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("NoCount",  pProposal->GetNoCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("AbstainCount",  pProposal->GetAbstainCount(VOTE_SIGNAL_FUNDING)));

            // REPORT VALIDITY AND CACHING FLAGS FOR VARIOUS SETTINGS
            std::string strError = "";
            bObj.push_back(Pair("fBlockchainValidity",  pProposal->IsValidLocally(strError, false)));
            bObj.push_back(Pair("IsValidReason",  strError.c_str()));
            bObj.push_back(Pair("fCachedValid",  pProposal->IsSetCachedValid()));
            bObj.push_back(Pair("fCachedFunding",  pProposal->IsSetCachedFunding()));
            bObj.push_back(Pair("fCachedDelete",  pProposal->IsSetCachedDelete()));
            bObj.push_back(Pair("fCachedEndorsed",  pProposal->IsSetCachedEndorsed()));

            objResult.push_back(Pair(pProposal->GetHash().ToString(), bObj));
        }

        return objResult;
    }

    // GET SPECIFIC GOVERNANCE ENTRY
    if(strCommand == "get")
    {
        if (params.size() != 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'smartvoting get <proposal-hash>'");

        // COLLECT VARIABLES FROM OUR USER
        uint256 hash = ParseHashV(params[1], "Proposal hash");

        LOCK2(cs_main, smartVoting.cs);

        // FIND THE GOVERNANCE OBJECT THE USER IS LOOKING FOR
        CProposal* pProposal = smartVoting.FindProposal(hash);

        if(pProposal == NULL)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown governance object");

        // REPORT BASIC OBJECT STATS

        UniValue objResult(UniValue::VOBJ);
        objResult.push_back(Pair("Hash",  pProposal->GetHash().ToString()));
        objResult.push_back(Pair("FeeHash",  pProposal->GetFeeHash().ToString()));
        objResult.push_back(Pair("CreationTime", pProposal->GetCreationTime()));
        const CSmartAddress& proposalAddress = pProposal->GetAddress();
        if(proposalAddress.IsValid()) {
            objResult.push_back(Pair("ProposalAddress", proposalAddress.ToString()));
        }else{
            objResult.push_back(Pair("ProposalAddress", "Invalid"));
        }

        // SHOW (MUCH MORE) INFORMATION ABOUT VOTES FOR GOVERNANCE OBJECT (THAN LIST/DIFF ABOVE)
        // -- FUNDING VOTING RESULTS

        UniValue objFundingResult(UniValue::VOBJ);
        objFundingResult.push_back(Pair("AbsoluteYesCount",  pProposal->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("YesCount",  pProposal->GetYesCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("NoCount",  pProposal->GetNoCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("AbstainCount",  pProposal->GetAbstainCount(VOTE_SIGNAL_FUNDING)));
        objResult.push_back(Pair("FundingResult", objFundingResult));

        // -- VALIDITY VOTING RESULTS
        UniValue objValid(UniValue::VOBJ);
        objValid.push_back(Pair("AbsoluteYesCount",  pProposal->GetAbsoluteYesCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("YesCount",  pProposal->GetYesCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("NoCount",  pProposal->GetNoCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("AbstainCount",  pProposal->GetAbstainCount(VOTE_SIGNAL_VALID)));
        objResult.push_back(Pair("ValidResult", objValid));

        // -- DELETION CRITERION VOTING RESULTS
        UniValue objDelete(UniValue::VOBJ);
        objDelete.push_back(Pair("AbsoluteYesCount",  pProposal->GetAbsoluteYesCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("YesCount",  pProposal->GetYesCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("NoCount",  pProposal->GetNoCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("AbstainCount",  pProposal->GetAbstainCount(VOTE_SIGNAL_DELETE)));
        objResult.push_back(Pair("DeleteResult", objDelete));

        // --
        std::string strError = "";
        objResult.push_back(Pair("fLocalValidity",  pProposal->IsValidLocally(strError, false)));
        objResult.push_back(Pair("IsValidReason",  strError.c_str()));
        objResult.push_back(Pair("fCachedValid",  pProposal->IsSetCachedValid()));
        objResult.push_back(Pair("fCachedFunding",  pProposal->IsSetCachedFunding()));
        objResult.push_back(Pair("fCachedDelete",  pProposal->IsSetCachedDelete()));
        objResult.push_back(Pair("fCachedEndorsed",  pProposal->IsSetCachedEndorsed()));
        return objResult;
    }

    // GETVOTES FOR SPECIFIC GOVERNANCE OBJECT
    if(strCommand == "getvotes")
    {
        if (params.size() != 2)
            throw std::runtime_error(
                "Correct usage is 'smartvoting getvotes <governance-hash>'"
                );

        // COLLECT PARAMETERS FROM USER

        uint256 hash = ParseHashV(params[1], "Governance hash");

        // FIND OBJECT USER IS LOOKING FOR

        LOCK(smartVoting.cs);

        CProposal* pProposal = smartVoting.FindProposal(hash);

        if(pProposal == NULL) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown governance-hash");
        }

        // REPORT RESULTS TO USER

        UniValue bResult(UniValue::VOBJ);

        // GET MATCHING VOTES BY HASH, THEN SHOW USERS VOTE INFORMATION

        std::vector<CProposalVote> vecVotes = smartVoting.GetMatchingVotes(hash);
        for (const auto& vote : vecVotes) {
            bResult.push_back(Pair(vote.GetHash().ToString(),  vote.ToString()));
        }

        return bResult;
    }

    return NullUniValue;
}

UniValue votekeys(const UniValue& params, bool fHelp)
{

    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    std::vector<std::string> vecCommands = {
        "register",
        "get",
        "count",
        "list"
    };

    if( std::find(vecCommands.begin(), vecCommands.end(), strCommand) == vecCommands.end() )
        throw std::runtime_error(
           "votekeys \"command\"...\n"
           "Use SmartProposal commands.\n"
           "\nAvailable commands:\n"
           "  register           - Register an SmartCash address for voting\n"
           "  getvotekey         - Get the registered votekey for an address\n"
           "  getaddress         - Get the address registered for a votekey\n"
           "  count              - Count all registered votekeys\n"
           "  list               - List all registered votekeys\n"
           );

    if(strCommand == "register")
    {
        if (params.size() != 4)  {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'votekeys register <address> <txhash> <index>' where <txhash> and <index> should describe an unspent output used to register with at least 1.002 SMART");
        }

        if( !pwalletMain )
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not available.");

        CVoteKey voteKey;
        unsigned char cRegisterOption = 0x01;

        uint256 txHash = uint256S(params[2].get_str());
        int64_t txIndex = params[3].get_int64();

        // **
        // Check if the unspent output belongs to <address> or not
        // **

        CTransaction spendTx;
        uint256 blockHash;

        if( !GetTransaction(txHash, spendTx, Params().GetConsensus(), blockHash, true) )
            throw JSONRPCError(RPC_INVALID_PARAMETER, "<txhash> doesn't belong to a transaction");

        if( txIndex < 0 || static_cast<int64_t>(spendTx.vout.size()) - 1 < txIndex )
            throw JSONRPCError(RPC_INVALID_PARAMETER, "<index> out of range");

        const CTxOut &utxo = spendTx.vout[txIndex];

        // **
        // Validate the given address
        // **

        CSmartAddress voteAddress(params[1].get_str());

        if ( !voteAddress.IsValid() )
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid address");

        CKeyID voteAddressKeyID;
        CKey vaKey;

        if (!voteAddress.GetKeyID(voteAddressKeyID))
            throw JSONRPCError(RPC_TYPE_ERROR, "<address> doesn't refer to key");

        if( GetVoteKeyForAddress(voteAddress, voteKey) ){
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Address is already registered for key: %s", voteKey.ToString()));
        }

        // If the given utxo is from the address to register use register option 1
        // Option 1 - verify the vote address with the input of the register tx
        // Option 2 - use a second signature in the op_return to verify the the vote address
        if( utxo.scriptPubKey == voteAddress.GetScript() )
            cRegisterOption = 0x01;
        else
            cRegisterOption = 0x02;

        // **
        // Get the private key of the address for option 2
        // **

        if( cRegisterOption == 0x02 && !pwalletMain->GetKey(voteAddressKeyID, vaKey) )
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key for <address> not available");

        // **
        // Generate a new voting key
        // **

        CKey secret;
        secret.MakeNewKey(false);
        CVoteKeySecret voteKeyPrivate(secret);

        CKey vkKey = voteKeyPrivate.GetKey();
        if (!vkKey.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Voting private key outside allowed range");

        CPubKey pubkey = vkKey.GetPubKey();
        if(!vkKey.VerifyPubKey(pubkey)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey verification failed");
        CKeyID vkKeyId = pubkey.GetID();
        voteKey.Set(vkKeyId);

        if( !voteKey.IsValid() ) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "VoteKey invalid");


        // Create the message to sign with the vote key and also voteaddress if required
        CDataStream ss(SER_GETHASH, 0);
        ss << strMessageMagic;
        ss << voteKey;
        ss << voteAddress;

        std::vector<unsigned char> vecSigAddress, vecSigVotekey;

        // Create the signature with the voting key
        if (!vkKey.SignCompact(Hash(ss.begin(), ss.end()), vecSigVotekey))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Signing with votekey failed");

        // And if required with the vote address
        if( cRegisterOption == 0x02 && !vaKey.SignCompact(Hash(ss.begin(), ss.end()), vecSigAddress))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Signing with votekey failed");

        std::vector<unsigned char> vecData = {
            OP_RETURN_VOTE_KEY_REG_FLAG,
            cRegisterOption
        };

        CDataStream registerData(SER_NETWORK,0);

        registerData << voteKey;
        registerData << vecSigVotekey;

        if( cRegisterOption == 0x02 ){
            registerData << voteAddress;
            registerData << vecSigAddress;
        }

        vecData.insert(vecData.end(), registerData.begin(), registerData.end());

        CScript registerScript = CScript() << OP_RETURN << vecData;

        // **
        // Create the transaction
        // **

        LOCK2(cs_main, pwalletMain->cs_wallet);

        EnsureWalletIsUnlocked();

        CCoinControl coinControl;
        COutPoint output(txHash, txIndex);

        CTxDestination change;

        if( cRegisterOption == 0x01 ){
            change = voteAddress.Get();
        }else{

            std::vector<CTxDestination> addresses;
            txnouttype type;
            int nRequired;

            if (!ExtractDestinations(utxo.scriptPubKey, type, addresses, nRequired) || addresses.size() != 1) {
                LogPrintf("ParseVoteKeyRegistration -- Couldn't extract address\n");
                return false;
            }

            change = addresses[0];
        }

        coinControl.fUseInstantSend = false;
        coinControl.Select(output);
        coinControl.destChange = change;

        // Create and send the transaction
        CWalletTx registerTx;
        CReserveKey reservekey(pwalletMain);
        CAmount nFeeRequired;
        std::string strError;
        vector<CRecipient> vecSend;
        int nChangePosRet = -1;

        CRecipient recipient = {registerScript, VOTEKEY_REGISTER_FEE, false};
        vecSend.push_back(recipient);

        if (!pwalletMain->CreateTransaction(vecSend, registerTx, reservekey, nFeeRequired, nChangePosRet,
                                             strError, &coinControl)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }

        CValidationState state;
        if (!(CheckTransaction(registerTx, state, registerTx.GetHash(), false) || !state.IsValid())){
            throw JSONRPCError(RPC_WALLET_ERROR, strprintf("The registration transaction is invalid: %s", state.GetRejectReason()));
        }

        if (!pwalletMain->CommitTransaction(registerTx, reservekey, g_connman.get())){
            throw JSONRPCError(RPC_WALLET_ERROR, "The transaction was rejected!");
        }

        UniValue result(UniValue::VOBJ);

        UniValue objTx(UniValue::VOBJ);
        TxToJSON(registerTx, uint256(), objTx);
        result.pushKV("registerTx", objTx);
        result.pushKV("voteAddress",voteAddress.ToString());
        result.pushKV("voteKey",voteKey.ToString());
        result.pushKV("voteKeySecret", voteKeyPrivate.ToString());

        return result;
    }

    if(strCommand == "get")
    {
        if (params.size() != 2)  {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'votekeys get <votekey/voteaddress>");
        }

        CVoteKey voteKey(params[1].get_str());
        CVoteKeyValue voteKeyValue;
        CSmartAddress voteAddress(params[1].get_str());

        if( !voteKey.IsValid() && !voteAddress.IsValid() )
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Parameter %s is neither a votekey nor a smartcash address", params[0].get_str()));

        if( voteAddress.IsValid() ){

            if( !GetVoteKeyForAddress(voteAddress, voteKey) )
                throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("No votekey found for address %s", voteAddress.ToString()));

        }

        if( voteKey.IsValid() ){

            if( !GetVoteKeyValue(voteKey, voteKeyValue) )
                throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("No votekey value entry found for votekey %s", voteKey.ToString()));

        }

        UniValue result(UniValue::VOBJ);

        result.pushKV("voteKey",voteKey.ToString());
        result.pushKV("voteAddress",voteKeyValue.voteAddress.ToString());
        result.pushKV("registerTx",voteKeyValue.nTxHash.ToString());
        result.pushKV("registerHeight", voteKeyValue.nBlockHeight);

        return result;
    }

    if(strCommand == "count")
    {
        std::vector<std::pair<CVoteKey,CVoteKeyValue>> vecVoteKeys;
        if( !GetVoteKeys(vecVoteKeys) )
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to load vote keys");

        return static_cast<int64_t>(vecVoteKeys.size());
    }

    if(strCommand == "list")
    {
        std::vector<std::pair<CVoteKey,CVoteKeyValue>> vecVoteKeys;
        if( !GetVoteKeys(vecVoteKeys) )
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to load vote keys");

        UniValue result(UniValue::VOBJ);

        for( auto vk : vecVoteKeys ){
            UniValue obj(UniValue::VOBJ);

            obj.pushKV("voteAddress",vk.second.voteAddress.ToString());
            obj.pushKV("registerTx",vk.second.nTxHash.ToString());
            obj.pushKV("registerHeight", vk.second.nBlockHeight);

            result.pushKV(vk.first.ToString(), obj);
        }

        return result;
    }

    return NullUniValue;
}
