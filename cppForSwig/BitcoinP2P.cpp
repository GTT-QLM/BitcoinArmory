////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <thread>
#include <chrono>
#include <ctime>
#include <string.h>
#include "BitcoinP2p.h"
#include "SocketWritePayload.h"

using namespace std;
using namespace Armory::Threading;

bool PEER_USES_WITNESS;

template <typename T> uint32_t put_integer_be(uint8_t* ptr, const T& integer)
{
   uint32_t size = sizeof(T);
   auto len = size - 1;
   auto intptr = (uint8_t*)&integer;

   for (uint32_t i = 0; i < size; i++)
   {
      ptr[i] = intptr[len - i];
   }

   return size;
};

////////////////////////////////////////////////////////////////////////////////
int make_varint(const uint64_t& value, vector<uint8_t>& varint)
{
   if (value < 0xFD)
   {
      varint.push_back((uint8_t)value);
      return 1;
   }
   else if (value <= 0xFFFF)
   {
      varint.resize(3);
      auto ptr = (uint16_t*)&varint[1];
      *ptr = (uint16_t)value;
      varint[0] = 0xFD;
      return 3;
   }
   else if (value <= 0xFFFFFFFF)
   {
      varint.resize(5);
      auto ptr = (uint32_t*)&varint[1];
      *ptr = (uint32_t)value;
      varint[0] = 0xFE;
      return 5;
   }

   varint.resize(9);
   auto ptr = (uint64_t*)&varint[1];
   *ptr = (uint64_t)value;
   varint[0] = 0xFF;
   return 9;
}

////////////////////////////////////////////////////////////////////////////////
int get_varint(uint64_t& val, uint8_t* ptr, uint32_t size)
{
   if (size == 0)
      throw runtime_error("invalid varint size");

   if (ptr[0] < 0xFD)
   {
      val = *(uint8_t*)(ptr);
      return 1;
   }
   else if (ptr[0] == 0xFD)
   {
      if (size < 3)
         throw runtime_error("invalid varint size");

      val = *(uint16_t*)(ptr + 1);
      return 3;
   }
   else if (ptr[0] == 0xFE)
   {
      if (size < 5)
         throw runtime_error("invalid varint size");

      val = *(uint32_t*)(ptr + 1);
      return 5;
   }

   if (size < 9)
      throw runtime_error("invalid varint size");

   val = *(uint64_t*)(ptr + 1);
   return 9;
}

////////////////////////////////////////////////////////////////////////////////
const map<string, PayloadType> BitcoinP2P::strToPayload_ = {
   make_pair("version", Payload_version),
   make_pair("verack", Payload_verack),
   make_pair("inv", Payload_inv),
   make_pair("ping", Payload_ping),
   make_pair("pong", Payload_pong),
   make_pair("getdata", Payload_getdata),
   make_pair("tx", Payload_tx),
   make_pair("reject", Payload_reject)
};

////////////////////////////////////////////////////////////////////////////////
////
//// Payload classes
////
////////////////////////////////////////////////////////////////////////////////
vector<uint8_t> Payload::serialize(uint32_t magic_word) const
{
   //serialize payload
   auto payload_size = serialize_inner(nullptr);
   
   vector<uint8_t> msg;
   msg.resize(MESSAGE_HEADER_LEN + payload_size);
   if (serialize(magic_word, &msg[0], msg.size()) == SIZE_MAX)
      throw BitcoinP2P_Exception("failed to serialize payload");

   return msg;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload::serialize(
   uint32_t magic_word, void* ptr, size_t buffer_len) const
{
   auto payload_size = serialize_inner(nullptr);
   if (buffer_len < payload_size + MESSAGE_HEADER_LEN)
      return SIZE_MAX;

   //message
   if (payload_size > 0)
      serialize_inner((uint8_t*)ptr + MESSAGE_HEADER_LEN);

   //magic word
   uint32_t* magicword = (uint32_t*)((uint8_t*)ptr + MAGIC_WORD_OFFSET);
   *magicword = magic_word;

   //message type
   auto&& type = typeStr();
   auto msgtype = (char*)ptr + MESSAGE_TYPE_OFFSET;
   memset(msgtype, 0, MESSAGE_TYPE_LEN);
   memcpy(msgtype, type.c_str(), type.size());

   //length
   uint32_t msglen = payload_size;
   uint32_t* msglenptr = (uint32_t*)((uint8_t*)ptr + PAYLOAD_LENGTH_OFFSET);
   *msglenptr = msglen;

   //checksum
   uint8_t* payloadptr = nullptr;
   if (payload_size > 0)
      payloadptr = (uint8_t*)ptr + MESSAGE_HEADER_LEN;
   BinaryDataRef bdr(payloadptr, payload_size);
   auto&& hash = BtcUtils::getHash256(bdr);
   uint32_t* checksum = (uint32_t*)hash.getPtr();
   uint32_t* checksumptr = (uint32_t*)((uint8_t*)ptr + CHECKSUM_OFFSET);
   *checksumptr = *checksum;

   return payload_size + MESSAGE_HEADER_LEN;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload::getSerializedSize() const
{
   auto payload_size = serialize_inner(nullptr);
   return MESSAGE_HEADER_LEN + payload_size;
}

////////////////////////////////////////////////////////////////////////////////
vector<size_t> Payload::processPacket(
   vector<uint8_t>& data, uint32_t magic_word)
{
   vector<size_t> retvec;   
   if (data.size() < MESSAGE_HEADER_LEN)
      return retvec;

   size_t offset = 0, totalsize = data.size();
   while (offset < totalsize)
   {
      uint8_t* ptr = &data[offset];

      //check magic word
      uint32_t* magicword = (uint32_t*)(ptr + MAGIC_WORD_OFFSET);
      if (*magicword != magic_word)
      {
         //invalid magic word, search remainder of the packet for another one
         auto sizeRemaining = totalsize - offset;
         auto mwFirstByte = *(uint8_t*)(&magic_word);
         unsigned i;
         for (i = 4; i < sizeRemaining; i++)
         {
            if (ptr[i] == mwFirstByte)
            {
               auto mwPtr = (uint32_t*)(ptr + i);
               if (*mwPtr == magic_word)
                  break;
            }
         }

         offset += i;
         continue;
      }

      //get message type
      char* messagetype = (char*)(ptr + MESSAGE_TYPE_OFFSET);

      //messagetype should be null terminated and no longer than 12 bytes
      int i;
      for (i = 0; i < MESSAGE_TYPE_LEN; i++)
      {
         if (messagetype[i] == 0)
            break;
      }

      if (i == MESSAGE_TYPE_LEN)
      {
         offset += 4; //skip the current mw before reentering the loop
         continue;
      }

      //get and verify length
      uint32_t* length = (uint32_t*)(ptr + PAYLOAD_LENGTH_OFFSET);
      auto localOffset = offset;

      //at this point we don't want to reparse this message if the
      //deser operation fails
      offset += 4;

      if (*length + MESSAGE_HEADER_LEN > totalsize - localOffset)
         return retvec;


      //get checksum
      uint32_t* checksum = (uint32_t*)(ptr + CHECKSUM_OFFSET);

      //grab payload
      BinaryDataRef payloadRef(ptr + MESSAGE_HEADER_LEN, *length);

      //verify checksum
      auto&& payloadHash = BtcUtils::getHash256(payloadRef);
      uint32_t* hashChecksum = (uint32_t*)payloadHash.getPtr();

      if (*hashChecksum != *checksum)
         continue;

      retvec.push_back(localOffset);
      offset += MESSAGE_HEADER_LEN + *length - 4;
   }

   return retvec;
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<Payload::DeserializedPayloads> Payload::deserialize(
   vector<uint8_t>& data, uint32_t magic_word,
   shared_ptr<DeserializedPayloads> prevPacket)
{
   size_t bytesConsumed = 0;

   auto parsepayloads = [&bytesConsumed](vector<uint8_t>& data, vector<size_t>& offsetVec)->
      shared_ptr<DeserializedPayloads>
   {
      auto result = make_shared<DeserializedPayloads>();

      auto& payloadVec = result->payloads_;

      for (unsigned y = 0; y < offsetVec.size(); y++)
      {
         auto offset = offsetVec[y];         
         bytesConsumed = offset;

         auto length = (uint32_t*)(&data[offset] + PAYLOAD_LENGTH_OFFSET);

         size_t localBytesConsumed = *length + MESSAGE_HEADER_LEN;
         if (localBytesConsumed + offset > data.size())
            break;

         auto messagetype = (char*)(&data[offset] + MESSAGE_TYPE_OFFSET);

         try
         {
            uint8_t* payloadptr = nullptr;
            if (*length > 0)
               payloadptr = &data[offset] + MESSAGE_HEADER_LEN;

            //instantiate relevant Payload child class and return it
            auto payloadIter = BitcoinP2P::strToPayload_.find(messagetype);
            if (payloadIter != BitcoinP2P::strToPayload_.end())
            {
               switch (payloadIter->second)
               {
               case Payload_version:
                  payloadVec.push_back(move(make_unique<Payload_Version>(
                     payloadptr, *length)));
                  break;

               case Payload_verack:
                  payloadVec.push_back(move(make_unique<Payload_Verack>()));
                  break;

               case Payload_ping:
                  payloadVec.push_back(move(make_unique<Payload_Ping>(
                     payloadptr, *length)));
                  break;

               case Payload_pong:
                  payloadVec.push_back(move(make_unique<Payload_Pong>(
                     payloadptr, *length)));
                  break;

               case Payload_inv:
                  payloadVec.push_back(move(make_unique<Payload_Inv>(
                     payloadptr, *length)));
                  break;

               case Payload_tx:
                  payloadVec.push_back(move(make_unique<Payload_Tx>(
                     payloadptr, *length)));
                  break;

               case Payload_getdata:
                  payloadVec.push_back(move(make_unique<Payload_GetData>(
                     payloadptr, *length)));
                  break;

               case Payload_reject:
                  payloadVec.push_back(move(make_unique<Payload_Reject>(
                     payloadptr, *length)));
                  break;

               default:
                  payloadVec.push_back(move(make_unique<Payload_Unknown>(
                     payloadptr, *length)));
               }
            }
            else
            {
               payloadVec.push_back(move(make_unique<Payload_Unknown>(
                  payloadptr, *length)));
            }

            bytesConsumed += localBytesConsumed;
         }
         catch (PayloadDeserError&)
         {
            continue;
         }
      }

      if (bytesConsumed < data.size())
      {
         result->spillOffset_ = bytesConsumed;
         result->data_ = move(data);
      }

      return result;
   };

   auto patchspill = [&parsepayloads, &bytesConsumed](
                     shared_ptr<DeserializedPayloads> prevpacket, 
                     const vector<uint8_t>& data,
                     const vector<size_t> offsets) -> shared_ptr<DeserializedPayloads>
   {
      if (prevpacket == nullptr)
         return nullptr;

      size_t spillSize = 0;
      if (offsets.size() == 0)
         spillSize = data.size();
      else
         spillSize = offsets[0];

      if (spillSize == 0)
      {
         return nullptr;
      }

      prevpacket->data_.insert(prevpacket->data_.end(),
         data.begin(), data.begin() + spillSize);

      vector<size_t> offvec;
      offvec.push_back(prevpacket->spillOffset_);

      auto spillResult = parsepayloads(prevpacket->data_, offvec);
      if (spillResult->spillOffset_ != SIZE_MAX)
      {
         spillResult->iterCount_ = prevpacket->iterCount_;
      }
      else
      {
         if (prevpacket->iterCount_ > 0)
            LOGWARN << "[[[ succesfully completed spill packet after " <<
               spillResult->iterCount_ << " iterations";
      }

      bytesConsumed += spillSize;
      return spillResult;
   };

   auto&& offsetVec = processPacket(data, magic_word);
   auto&& extraPacket = patchspill(prevPacket, data, offsetVec);

   auto result = parsepayloads(data, offsetVec);
   if (extraPacket != nullptr)
   {
      if (result->payloads_.size() == 0 && result->spillOffset_ == SIZE_MAX)
      {
         //LOGWARN << "returning extraPacket with iter: " << extraPacket->iterCount_;
         return extraPacket;
      }

      typedef vector<unique_ptr<Payload>>::iterator vecUP;

      vector<unique_ptr<Payload>> newvec;
      newvec.insert(newvec.end(), 
         move_iterator<vecUP>(extraPacket->payloads_.begin()),
         move_iterator<vecUP>(extraPacket->payloads_.end()));

      newvec.insert(newvec.end(),
         move_iterator<vecUP>(result->payloads_.begin()),
         move_iterator<vecUP>(result->payloads_.end()));

      result->payloads_ = move(newvec);
      result->iterCount_ = extraPacket->iterCount_;

      if (extraPacket->spillOffset_ != SIZE_MAX)
      {
         LOGWARN << "*** got valid payloads without completing spill packet";
         LOGWARN << "*** dumping " << extraPacket->data_.size() << " bytes of spill data";
      }
   }

   return result;
}


////////////////////////////////////////////////////////////////////////////////
void BitcoinNetAddr::deserialize(BinaryRefReader brr)
{
   if (brr.getSize() != NETADDR_NOTIME)
      throw PayloadDeserError("invalid netaddr size");

   services_ = brr.get_uint64_t(); 
   auto ipv6bdr = brr.get_BinaryDataRef(16);
   memcpy(&ipV6_, ipv6bdr.getPtr(), 16);

   port_ = brr.get_uint16_t();
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNetAddr::serialize(uint8_t* ptr) const
{
   memcpy(ptr, &services_, 8);
   memcpy(ptr + 8, ipV6_, 16);
   put_integer_be(ptr + 24, port_);
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Unknown::deserialize(uint8_t* data, size_t len)
{
   data_.clear();
   if (len == 0)
      return;

   data_.resize(len);
   memcpy(&data_[0], data, len);
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Unknown::serialize_inner(uint8_t* ptr) const
{
   if (ptr != nullptr && data_.size() > 0)
      memcpy(ptr, &data_[0], data_.size());

   return data_.size();   
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Version::deserialize(uint8_t* data, size_t len)
{
   uint8_t* dataptr = data;

   vheader_.version_ = *(uint32_t*)dataptr;
   dataptr += 4;
   vheader_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   vheader_.timestamp_ = *(int64_t*)dataptr;
   dataptr += 8;

   vheader_.addr_recv_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   memcpy(vheader_.addr_recv_.ipV6_, dataptr, 16);
   dataptr += 16;
   vheader_.addr_recv_.port_ = *(uint16_t*)dataptr;
   dataptr += 2;

   vheader_.addr_from_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   memcpy(vheader_.addr_from_.ipV6_, dataptr, 16);
   dataptr += 16;
   vheader_.addr_from_.port_ = *(uint16_t*)dataptr;
   dataptr += 2;

   vheader_.nonce_ = *(uint64_t*)dataptr;
   dataptr += 8;

   size_t remaining = len - USERAGENT_OFFSET;
   uint64_t uaLen;
   auto varintlen = get_varint(uaLen, dataptr, remaining);
   dataptr += varintlen;
   char* userAgentPtr = (char*)(dataptr);
   userAgent_ = string(userAgentPtr, uaLen);
   dataptr += uaLen;

   startHeight_ = *(uint32_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Version::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      return BtcUtils::get_varint_len(userAgent_.size()) +
         userAgent_.size() +
         VERSION_MINLENGTH;
   }

   vector<uint8_t> varint;
   auto varintlen = make_varint(userAgent_.size(), varint);
   size_t serlen = varintlen + userAgent_.length() + VERSION_MINLENGTH;

   uint8_t* vhptr = dataptr;
   
   memcpy(vhptr, &vheader_.version_, 4);
   memcpy(vhptr +4, &vheader_.services_, 8);
   memcpy(vhptr + 12, &vheader_.timestamp_, 8);
   vhptr += 20;

   vheader_.addr_recv_.serialize(vhptr);
   vhptr += 26;

   vheader_.addr_from_.serialize(vhptr);
   vhptr += 26;

   memcpy(vhptr, &vheader_.nonce_, 8);

   uint8_t* ptr = dataptr + USERAGENT_OFFSET;
   memcpy(ptr, &varint[0], varintlen);
   ptr += varintlen;
   memcpy(ptr, userAgent_.c_str(), userAgent_.size());
   ptr += userAgent_.size();
   memcpy(ptr, &startHeight_, 4);
   ptr += 4;
   *ptr = 1;
   
   return serlen;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Version::setVersionHeaderIPv4(uint32_t version, uint64_t services,
   int64_t timestamp,
   const sockaddr& recvaddr,
   const sockaddr& fromaddr)
{
   vheader_.version_ = version;
   vheader_.services_ = services;
   vheader_.timestamp_ = timestamp;

   vheader_.addr_recv_.setIPv4(services, recvaddr);
   vheader_.addr_from_.setIPv4(services, fromaddr);

   auto&& randombytes = BtcUtils::fortuna_.generateRandom(8);
   vheader_.nonce_ = *(uint64_t*)randombytes.getPtr();
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Ping::deserialize(uint8_t* dataptr, size_t len)
{
   if (len == 0)
      nonce_ = UINT64_MAX;
   else if (len != 8)
      throw PayloadDeserError("invalid ping payload len");
   else
      nonce_ = *(uint64_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Ping::serialize_inner(uint8_t* dataptr) const
{
   if (nonce_ == UINT64_MAX)
      return 0;
   
   if (dataptr == nullptr)
      return 8;

   uint64_t* ptr = (uint64_t*)dataptr;
   *ptr = nonce_;

   return 8;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Pong::deserialize(uint8_t* dataptr, size_t len)
{
   if (len != 8)
      throw PayloadDeserError("invalid pong payload len");

   nonce_ = *(uint64_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Pong::serialize_inner(uint8_t* dataptr) const
{
   if (nonce_ == UINT64_MAX)
      return 0;

   if (dataptr == nullptr)
      return 8;

   uint64_t* ptr = (uint64_t*)dataptr;
   *ptr = nonce_;

   return 8;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Inv::deserialize(uint8_t* dataptr, size_t len)
{
   uint64_t invCount;
   auto varintlen = get_varint(invCount, dataptr, len);

   if (invCount > INV_MAX)
      throw PayloadDeserError("inv count > INV_MAX");

   invVector_.resize(invCount);

   auto ptr = dataptr + varintlen;
   auto remaining = len - varintlen;

   for (auto& entry : invVector_)
   {
      if (remaining < INV_ENTRY_LEN)
         throw PayloadDeserError("inv deser size mismatch");

      auto entrytype = *(uint32_t*)(ptr);
      if (entrytype > 3)
         throw PayloadDeserError("invalid inv entry type");

      entry.invtype_ = (InvType)entrytype;
      memcpy(entry.hash, ptr + 4, 32);

      remaining -= INV_ENTRY_LEN;
      ptr += INV_ENTRY_LEN;
   }
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Tx::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
      return rawTx_.size();

   memcpy(dataptr, &rawTx_[0], rawTx_.size());
   return rawTx_.size();
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Tx::deserialize(uint8_t* dataptr, size_t len)
{
   rawTx_.resize(len);
   memcpy(&rawTx_[0], dataptr, len);
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Inv::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      auto invcount = invVector_.size();
      auto varintlen = BtcUtils::get_varint_len(invcount);

      return invcount * INV_ENTRY_LEN + varintlen;
   }

   auto invcount = invVector_.size();
   vector<uint8_t> varint;
   auto varintlen = make_varint(invcount, varint);

   memcpy(dataptr, &varint[0], varintlen);
   dataptr += varintlen;

   for (auto& entry : invVector_)
   {
      auto intptr = (uint32_t*)dataptr;
      *intptr = (uint32_t)entry.invtype_;
      memcpy(dataptr + 4, entry.hash, 32);

      dataptr += 36;
   }

   return varintlen + invcount * INV_ENTRY_LEN;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_GetData::deserialize(uint8_t* dataptr, size_t len)
{
   uint64_t invCount;
   auto varintlen = get_varint(invCount, dataptr, len);

   if (invCount > INV_MAX)
      throw PayloadDeserError("inv count > INV_MAX");

   invVector_.resize(invCount);

   auto ptr = dataptr + varintlen;
   auto remaining = len - varintlen;

   for (auto& entry : invVector_)
   {
      if (remaining < INV_ENTRY_LEN)
         throw PayloadDeserError("inv deser size mismatch");

      auto entrytype = *(uint32_t*)(ptr);
      if ((entrytype & ~Inv_Witness) > 3)
         throw PayloadDeserError("invalid inv entry type");

      entry.invtype_ = (InvType)entrytype;
      memcpy(entry.hash, ptr + 4, 32);

      remaining -= INV_ENTRY_LEN;
      ptr += INV_ENTRY_LEN;
   }
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_GetData::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      auto invcount = invVector_.size();
      auto varintlen = BtcUtils::get_varint_len(invcount);

      return invcount * INV_ENTRY_LEN + varintlen;
   }

   auto invcount = invVector_.size();
   vector<uint8_t> varint;
   auto varintlen = make_varint(invcount, varint);

   memcpy(dataptr, &varint[0], varintlen);
   dataptr += varintlen;

   for (auto& entry : invVector_)
   {
      auto intptr = (uint32_t*)dataptr;
      *intptr = (uint32_t)entry.invtype_;
      memcpy(dataptr + 4, entry.hash, 32);

      dataptr += 36;
   }

   return varintlen + invcount * INV_ENTRY_LEN;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Reject::deserialize(uint8_t* dataptr, size_t len)
{
   uint64_t typeLen;

   //message field size in bytes
   auto varintlen = get_varint(typeLen, dataptr, len);
   auto ptr = dataptr + varintlen;

   //message type
   string msgtype((char*)ptr, typeLen);
   auto typeIter = BitcoinP2P::strToPayload_.find(msgtype);
   if (typeIter == BitcoinP2P::strToPayload_.end())
      throw PayloadDeserError("unknown reject type");

   rejectType_ = typeIter->second;
   ptr += typeLen;

   //reject code as integer
   code_ = (char)*ptr;
   ptr++;

   auto reasonOffset = typeLen + varintlen + 1;

   //reason str size
   uint64_t reasonLen;
   varintlen = get_varint(reasonLen, ptr, len);
   ptr += varintlen;
   
   //reason str
   reasonStr_ = move(string((char*)ptr, reasonLen));
   ptr += reasonLen;

   //extra data, final field. size and processing depends on reject code
   //just copy the data, handle when processing the payload
   auto remaining = len - (reasonLen + varintlen + reasonOffset);
   if (remaining == 0)
      return;

   extra_.resize(remaining);
   memcpy(&extra_[0], ptr, remaining);
}

////////////////////////////////////////////////////////////////////////////////
////
//// BitcoinNodeInterface
////
////////////////////////////////////////////////////////////////////////////////
BitcoinNodeInterface::BitcoinNodeInterface(uint32_t magic_word, bool watcher) :
   magic_word_(magic_word)
{
   if (!watcher)
      invBlockStack_ = make_shared<BlockingQueue<vector<InvEntry>>>();
}

////////////////////////////////////////////////////////////////////////////////
BitcoinNodeInterface::~BitcoinNodeInterface()
{
   shutdown();
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::shutdown()
{
   if (!run_.load(memory_order_relaxed))
      return;

   //clean up remaining lambdas
   if (invBlockStack_ != nullptr)
      invBlockStack_->terminate();
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::processInvBlock(vector<InvEntry> invVec)
{
   if (invBlockStack_ != nullptr)
      invBlockStack_->push_back(move(invVec));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::registerInvTxLambda(
   function<void(vector<InvEntry>)> func)
{
   invTxLambda_ = move(func);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::registerNodeStatusLambda(function<void(void)> lbd)
{ 
   nodeStatusLambda_ = lbd; 
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::registerGetTxCallback(
   const std::function<void(std::unique_ptr<Payload>)>& lbd)
{
   getTxDataLambda_ = lbd;
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::processInvTx(vector<InvEntry> invVec)
{
   if (invTxLambda_)
      invTxLambda_(invVec);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::processGetTx(unique_ptr<Payload> payload)
{
   if (getTxDataLambda_)
      getTxDataLambda_(move(payload));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNodeInterface::requestTx(vector<InvEntry> invVec)
{
   /*
   Send getdata payload to bitcoin node to request transactions. Node
   reply will be processed in processGetTx
   */

   for (auto& entry : invVec)
   {
      if (entry.invtype_ != Inv_Msg_Tx && entry.invtype_ != Inv_Msg_Witness_Tx)
        throw GetDataException("entry type isnt Inv_Msg_Tx");
   }

   auto payload = make_unique<Payload_GetData>(move(invVec));
   sendMessage(move(payload));
}

////////////////////////////////////////////////////////////////////////////////
////
//// BitcoinP2P
////
////////////////////////////////////////////////////////////////////////////////
BitcoinP2P::BitcoinP2P(
   const string& addrV4, const string& port,
   uint32_t magicword, bool watcher) :
   BitcoinNodeInterface(magicword, watcher), addr_(addrV4), port_(port)
{
   init();
}

////////////////////////////////////////////////////////////////////////////////
BitcoinP2P::~BitcoinP2P()
{
   if (invBlockStack_ != nullptr)
      invBlockStack_->terminate();
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::init()
{
   nodeConnected_.store(false, memory_order_relaxed);
   run_.store(true, memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::connectToNode(bool async)
{
   unique_lock<mutex> lock(connectMutex_, defer_lock);

   if (!lock.try_lock()) //return if another thread is already here
      throw SocketError("another connect attempt is underway");

   connectedPromise_ = unique_ptr<promise<bool>>(new promise<bool>());
   auto connectedFuture = connectedPromise_->get_future();

   auto connectLambda = [this](void)->void
   {
      this->connectLoop();
   };

   thread connectthread(connectLambda);
   if (connectthread.joinable())
      connectthread.detach();

   if (async)
      return;

   connectedFuture.get();

   if (select_except_ != nullptr)
      rethrow_exception(select_except_);

   if (process_except_ != nullptr)
      rethrow_exception(process_except_);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::connectLoop(void)
{
   size_t waitBeforeReconnect = 0;
   promise<bool> shutdownPromise;
   shutdownFuture_ = shutdownPromise.get_future();

   if (!invTxLambda_)
      throw SocketError("BitcoinP2P object is not initialized");

   while (run_.load(memory_order_relaxed))
   {
      //clean up stacks
      dataStack_ = make_shared<BlockingQueue<vector<uint8_t>>>();
      socket_ = make_unique<BitcoinP2PSocket>(addr_, port_, dataStack_);

      verackPromise_ = make_unique<promise<bool>>();
      auto verackFuture = verackPromise_->get_future();

      while (run_.load(memory_order_relaxed))
      {
         if (socket_->openSocket(false))
            break;

         if (waitBeforeReconnect < 5000)
            waitBeforeReconnect += RECONNECT_INCREMENT_MS;

         this_thread::sleep_for(chrono::milliseconds(waitBeforeReconnect));
      }

      auto processThread = [this](void)->void
      {
         try
         {
            this->processDataStackThread();
         }
         catch (...)
         {
            this->process_except_ = current_exception();
         }
      };

      socket_->connectToRemote();
      thread processThr(processThread);

      //send version payload
      auto version = make_unique<Payload_Version>();
      auto timestamp = getTimeStamp();

      struct sockaddr clientsocketaddr;

      try
      {
         //send version
         if (socket_->getSocketName(clientsocketaddr) != 0)
            throw SocketError("failed to get client sockaddr");

         if (socket_->getPeerName(node_addr_) != 0)
            throw SocketError("failed to get peer sockaddr");

         // Services, for future extensibility
         uint32_t services = NODE_WITNESS;

         version->setVersionHeaderIPv4(70012, services, timestamp,
            node_addr_, clientsocketaddr);

         version->userAgent_ = "Armory:0.96.5";
         version->startHeight_ = -1;

         sendMessage(move(version));

         //wait on verack
         verackFuture.get();
         verackPromise_.reset();
         LOGINFO << "Connected to Bitcoin node";
         updateNodeStatus(true);

         //signal calling thread
         connectedPromise_->set_value(true);
         waitBeforeReconnect = 0;

         //signal new blocks for good measure
         processInvBlock(move(vector<InvEntry>()));
      }
      catch (...)
      {
         waitBeforeReconnect += RECONNECT_INCREMENT_MS;
         this_thread::sleep_for(chrono::milliseconds(waitBeforeReconnect));
      }

      //wait on threads
      if (processThr.joinable())
         processThr.join();
      
      //close socket to guarantee select returns
      if (socket_->isValid())
         socket_->shutdown();

      LOGINFO << "Disconnected from Bitcoin node";
      updateNodeStatus(false);
   }

   shutdownPromise.set_value(true);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processDataStackThread()
{
   try
   {
      shared_ptr<Payload::DeserializedPayloads> packetPtr;

      while (1)
      {
         shared_ptr<Payload::DeserializedPayloads> prevPacket = packetPtr;
         packetPtr.reset();

         auto&& data = dataStack_->pop_front();
         auto&& processedPacket = Payload::deserialize(
            data, getMagicWord(), prevPacket);

         if (processedPacket->spillOffset_ != SIZE_MAX)
         {
            packetPtr = processedPacket;
         }

         processPayload(move(processedPacket->payloads_));
      }
   }
   catch (StopBlockingLoop&)
   {
      LOGERR << "caught StopBlockingLoop in processDataStackThread";
   }
   catch (BitcoinP2P_Exception& e)
   {
      LOGERR << "caught BitcoinP2P_Exception in processDataStackThread: "
         << e.what();
   }
   catch (exception& e)
   {
      LOGERR << "caught exception in processDataStackThread: "
         << e.what();
   }
   catch (...)
   {

      LOGERR << "caught unkown exception in processDataStackThread";
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processPayload(vector<unique_ptr<Payload>> payloadVec)
{
   for (auto&& payload : payloadVec)
   {
      switch (payload->type())
      {
      case Payload_version:
         checkServices(move(payload));
         returnVerack();
         break;

      case Payload_verack:
         gotVerack();
         break;

      case Payload_ping:
         replyPong(move(payload));
         break;

      case Payload_inv:
         processInv(move(payload));
         break;

      case Payload_getdata:
         processGetData(move(payload));
         break;

      case Payload_tx:
         processGetTx(move(payload));
         break;

      case Payload_reject:
         processReject(move(payload));
         break;

      default:
         continue;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::checkServices(unique_ptr<Payload> payload)
{
   Payload_Version* pver = (Payload_Version*)payload.get();

   auto& this_mw = Armory::Config::BitcoinSettings::getMagicBytes();
   auto mwInt = (uint32_t*)this_mw.getPtr();

   auto magicWord = getMagicWord();
   if(*mwInt != magicWord)
   {
      BinaryDataRef bdr_mw;
      bdr_mw.setRef((uint8_t*)&magicWord, 4);

      LOGERR << "Node magic word does not match expected magic word:";
      LOGERR << "   expected: " << this_mw.toHexStr();
      LOGERR << "   got: " << bdr_mw.toHexStr();
      throw BitcoinP2P_Exception("magic word mismatch");
   }

   if(pver->vheader_.services_ & NODE_WITNESS)
      PEER_USES_WITNESS = true;
   else
      PEER_USES_WITNESS = false;

   topBlock_ = pver->startHeight_;
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::gotVerack(void)
{
   if (verackPromise_ == nullptr)
      return;

   try
   {
      verackPromise_->set_value(true);
   }
   catch (future_error&)
   {
      //already set or no shared state, move on
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::returnVerack(void)
{
   auto verack = make_unique<Payload_Verack>();
   sendMessage(move(verack));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::replyPong(unique_ptr<Payload> payload)
{
   Payload_Ping* pping = (Payload_Ping*)payload.get();
   auto ppong = make_unique<Payload_Pong>();

   ppong->nonce_ = pping->nonce_;
   sendMessage(move(ppong));
}


////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processInv(unique_ptr<Payload> payload)
{
   Payload_Inv* invptr = (Payload_Inv*)payload.get();

   //order entries by type
   map<InvType, vector<InvEntry>> orderedEntries;
   
   for (auto& entry : invptr->invVector_)
   {
      auto& invvec = orderedEntries[entry.invtype_];
      invvec.push_back(move(entry));
   }

   //process them
   for (auto& entryVec : orderedEntries)
   {
      switch (entryVec.first)
      {
      case Inv_Msg_Witness_Block:
      case Inv_Msg_Block:
      {
         //1 sec delay to make sure data is written on disk
         this_thread::sleep_for(chrono::seconds(1));
         processInvBlock(move(entryVec.second));
         break;
      }

      case Inv_Msg_Witness_Tx:
      case Inv_Msg_Tx:         
         processInvTx(move(entryVec.second));
         break;

      default:
         continue;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processGetData(unique_ptr<Payload> payload)
{
   auto payloadgetdata = (Payload_GetData*)payload.get();

   auto& invvector = payloadgetdata->getInvVector();
   auto getdatamap = getDataPayloadMap_.get();

   vector<unique_ptr<Payload>> payloadVec;
   for (auto& entry : invvector)
   {
      BinaryDataRef bdr(entry.hash, 32);

      auto payloadIter = getdatamap->find(bdr);
      if (payloadIter == getdatamap->end())
         continue;
      
      payloadVec.push_back(move(payloadIter->second->payload_));
      getDataPayloadMap_.erase(payloadIter->first);
   }

   sendMessage(move(payloadVec));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processReject(unique_ptr<Payload> payload)
{
   if (payload->type() != Payload_reject)
   {
      LOGERR << "processReject: expected payload_reject type, got " <<
         payload->typeStr() << " instead";
      return;
   }

   processGetTx(move(payload));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::sendMessage(unique_ptr<Payload> payload)
{
   auto&& msg = payload->serialize(getMagicWord());

   unique_lock<mutex> lock(writeMutex_);
   auto socket_payload = make_unique<WritePayload_Raw>();
   socket_payload->data_ = move(msg);
   socket_->pushPayload(move(socket_payload), nullptr);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::sendMessage(vector<unique_ptr<Payload>> payloadVec)
{
   vector<uint8_t> msg;
   size_t totalSize = 0;
   for (auto& payload : payloadVec)
      totalSize += payload->getSerializedSize();

   msg.resize(totalSize);
   size_t offset = 0;
   for (auto& payload : payloadVec)
   {
      offset += payload->serialize(
         getMagicWord(), &msg[0] + offset, msg.size() - offset);
   }

   unique_lock<mutex> lock(writeMutex_);
   auto socket_payload = make_unique<WritePayload_Raw>();
   socket_payload->data_ = move(msg);
   socket_->pushPayload(move(socket_payload), nullptr);
}

////////////////////////////////////////////////////////////////////////////////
int64_t BitcoinP2P::getTimeStamp() const
{
   return (int64_t)time(0);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::shutdown()
{
   if (!run_.load(memory_order_relaxed))
      return;

   if (socket_ != nullptr)
   {
      socket_->shutdown();

      //wait until connect loop exists
      shutdownFuture_.wait();
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::updateNodeStatus(bool connected)
{
   nodeConnected_.store(connected, memory_order_release);
   callback();
}

////////////////////////////////////////////////////////////////////////////////
//
// BitcoinP2PSocket
//
////////////////////////////////////////////////////////////////////////////////
void  BitcoinP2PSocket::respond(vector<uint8_t>& packet)
{

   if (packet.size() > 0)
      readDataStack_->push_back(move(packet));
   else
      readDataStack_->terminate();
}


////////////////////////////////////////////////////////////////////////////////
void BitcoinP2PSocket::pushPayload(
   unique_ptr<Socket_WritePayload> write_payload,
   shared_ptr<Socket_ReadPayload>)
{
   if (write_payload == nullptr)
      return;

   vector<uint8_t> data;
   write_payload->serialize(data);
   queuePayloadForWrite(data);
}
