/* @(#)root/multiproc:$Id$ */
// Author: Enrico Guiraud July 2015

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TMPWorker
#define ROOT_TMPWorker

#include "TSysEvtHandler.h" //TFileHandler
#include "TSocket.h"
#include "TBufferFile.h"
#include "TClass.h"
#include "MPSendRecv.h"
#include <unistd.h> //pid_t
#include <string>
#include <memory> //unique_ptr

class TMPWorker : public TFileHandler {
   /// \cond
   ClassDef(TMPWorker, 0);
   /// \endcond
public:
   TMPWorker();
   virtual ~TMPWorker() {};
   //it doesn't make sense to copy a TMPWorker (each one has a uniq_ptr to its socket)
   TMPWorker(const TMPWorker&) = delete;
   TMPWorker& operator=(const TMPWorker&) = delete;

   virtual void Init(int fd); 
   inline TSocket* GetSocket() { return fS.get(); }
   inline pid_t GetPid() { return fPid; }

private:
   virtual void HandleInput(MPCodeBufPair& msg);
   Bool_t Notify();
   Bool_t ReadNotify() { return Notify(); }

   std::unique_ptr<TSocket> fS; ///< This worker's socket
   pid_t fPid; ///< the PID of the process in which this worker is running
};

#endif
