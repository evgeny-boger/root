#include "TMPWorker.h"
#include "MPCode.h"
#include "MPSendRecv.h"
#include "TSystem.h"
#include <string>
#include <iostream>
#include <memory> //unique_ptr

//////////////////////////////////////////////////////////////////////////
///
/// \class TMPWorker
///
/// This class works in conjuction with TMPClient, reacting to messages
/// received from it as specified by the Notify and HandleInput methods.
/// When TMPClient::Fork is called, a TMPWorker instance is passed to it
/// which will take control of the ROOT session in the children processes.
///
/// After forking, every time a message is sent or broadcast to the workers,
/// TMPWorker::Notify is called and the message is retrieved.
/// Messages exchanged between TMPClient and TMPWorker should be sent with
/// the MPSend() standalone function.\n
/// If the code of the message received is above 1000 (i.e. it is an MPCode)
/// the qualified TMPWorker::HandleInput method is called, that takes care
/// of handling the most generic type of messages. Otherwise the unqualified
/// (possibly overridden) version of HandleInput is called, allowing classes
/// that inherit from TMPWorker to manage their own protocol.\n
/// An application's worker class should inherit from TMPWorker and implement
/// a HandleInput method that overrides TMPWorker's.\n
///
//////////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////////
/// Class constructor.
/// Note that this does not set variables like fPid or fS (worker's socket).\n
/// These operations are handled by the Init method, which is called after
/// forking.\n
/// This separation is in place because the instantiation of a worker
/// must be done once _before_ forking, while the initialization of the
/// members must be done _after_ forking by each of the children processes.
TMPWorker::TMPWorker() : TFileHandler(-1, kRead), fS(), fPid(0)
{
}


//////////////////////////////////////////////////////////////////////////
/// This method is called by children processes right after forking.
/// Initialization of worker properties that must be delayed until after
/// forking must be done here.\n
/// For example, Init saves the pid into fPid, and adds the TMPWorker to
/// the main eventloop (as a TFileHandler).\n
/// Make sure this operations are performed also by overriding implementations,
/// e.g. by calling TMPWorker::Init explicitly.
void TMPWorker::Init(int fd)
{
   fS.reset(new TSocket(fd,"MPsock")); //TSocket's constructor with this signature seems much faster than TSocket(int fd)
   fPid = getpid();

   //TFileHandler's stuff
   //these operations _must_ be done in the overriding implementations too
   SetFd(fd);
   Add();
}


//////////////////////////////////////////////////////////////////////////
/// Handle a message with an EMPCode.
/// This method is called upon receiving a message with a code >= 1000 (i.e.
/// EMPCode). It handles the most generic types of messages.\n
/// Classes inheriting from TMPWorker should implement their own HandleInput
/// function, that should be able to handle codes specific to that application.\n
/// The appropriate version of the HandleInput method (TMPWorker's or the 
/// overriding version) is automatically called depending on the message code.
void TMPWorker::HandleInput(MPCodeBufPair& msg)
{
   unsigned code = msg.first;
   
   std::string reply = "S" + std::to_string(fPid);
   if (code == MPCode::kMessage) {
      //general message, ignore it
      reply += ": ok";
      MPSend(fS.get(), MPCode::kMessage, reply.data());
   } else if (code == MPCode::kError) {
      //general error, ignore it
      reply += ": ko";
      MPSend(fS.get(), MPCode::kMessage, reply.data());
   } else if (code == MPCode::kShutdownOrder || code == MPCode::kFatalError) {
      //client is asking the server to shutdown or client is dying
      MPSend(fS.get(), MPCode::kShutdownNotice, reply.data());
      gSystem->Exit(0);
   } else {
      reply += ": unknown code received. code=" + std::to_string(code);
      MPSend(fS.get(), MPCode::kError, reply.data());
   }
}

//////////////////////////////////////////////////////////////////////////
/// This method is called by TFileHandler when there's an event on the TSocket fS.
/// It checks what kind of message was received (if any) and calls the appropriate
/// handler function (TMPWorker::HandleInput or overridden version).
Bool_t TMPWorker::Notify()
{
   MPCodeBufPair msg = MPRecv(fS.get());
   if(msg.first == MPCode::kRecvError) {
      std::cerr << "Lost connection to client\n";
      gSystem->Exit(0);
   }
   if(msg.first < 1000)
      HandleInput(msg); //call overridden method
   else
      TMPWorker::HandleInput(msg); //call this class' method

   return kTRUE;
}
