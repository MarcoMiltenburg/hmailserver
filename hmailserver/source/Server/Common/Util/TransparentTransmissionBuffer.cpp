// Copyright (c) 2005 Martin Knafve / hMailServer.com.  
// http://www.hmailserver.com
// Created 2005-10-05

#include "StdAfx.h"
#include ".\transparenttransmissionbuffer.h"

#include "ByteBuffer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   TransparentTransmissionBuffer::TransparentTransmissionBuffer(bool bSending) : 
      is_sending_(bSending),
      transmission_ended_(false),
      last_send_ended_with_newline_(false),
      data_sent_(0),
      max_size_kb_(0),
      cancel_transmission_(false)
   {
      buffer_ = std::shared_ptr<ByteBuffer>(new ByteBuffer);
   }

   TransparentTransmissionBuffer::~TransparentTransmissionBuffer(void)
   {

   }

   bool
   TransparentTransmissionBuffer::Initialize(std::weak_ptr<TCPConnection> pTCPConnection)
   {
      tcp_connection_ = pTCPConnection;

      data_sent_ = 0;

      return true;
   }

   bool 
   TransparentTransmissionBuffer::Initialize(const String &sFilename)
   {
      if (!file_.Open(sFilename, File::OTAppend))
      {
         // This is not good. We failed to get a handle to the file.
         // Log to event log and notify the sender of this error.

         String sErrorMessage;
         sErrorMessage.Format(_T("Failed to write to the file %s. Data from sender rejected."), sFilename.c_str());

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5075, "TransparentTransmissionBuffer::SaveToFile_", sErrorMessage);

         return false;
      } 

      data_sent_ = 0;

      return true;
   }

   void 
   TransparentTransmissionBuffer::SetMaxSizeKB(int maxSize)
   {
      max_size_kb_ = maxSize;
   }

   void 
   TransparentTransmissionBuffer::Append(const BYTE *pBuffer, int iBufferSize)
   {
      if (iBufferSize == 0)
      {
         // Nothing to add.
         return;
      }   

      if (pBuffer == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5411, "TransparentTransmissionBuffer::Append", "pBuffer is NULL");
         throw;
      }

      if (buffer_ == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5412, "TransparentTransmissionBuffer::Append", "buffer_ is NULL");
         throw;
      }

      data_sent_+= iBufferSize;

      // Add the new data to the buffer.
      buffer_->Add(pBuffer, iBufferSize);

      // Check if we have received the entire buffer.
      if (buffer_->GetSize() >= 3 && !is_sending_)
      {
         // If receiving, we should check for end-of-data
         int iSize = buffer_->GetSize();
         const char *pCharBuffer = buffer_->GetCharBuffer();

         // Check if the buffer only contains a dot on an empty line.
         bool bDotCRLFOnEmptyLine = (pCharBuffer[0] == '.' && pCharBuffer[1] == '\r' && pCharBuffer[2] == '\n');

         // Look for \r\n.\r\n. 
         bool bLineBeginnningWithDotCRLF = buffer_->GetSize() >= 5 &&
            (pCharBuffer[iSize -5] == '\r' && 
            pCharBuffer[iSize -4] == '\n' && 
            pCharBuffer[iSize -3] == '.' && 
            pCharBuffer[iSize -2] == '\r' && 
            pCharBuffer[iSize -1] == '\n');

         if (bDotCRLFOnEmptyLine || bLineBeginnningWithDotCRLF)
         {
            // Remove the transmission-end characters. (the 3 last)
            buffer_->DecreaseSize(3);

            transmission_ended_ = true;
         }
      }
   }

   bool 
   TransparentTransmissionBuffer::GetRequiresFlush()
   {
      if (buffer_->GetSize() > 40000 || transmission_ended_)
         return true;
      else
         return false;
   }

   int 
   TransparentTransmissionBuffer::GetSize()
   {  
      return data_sent_;
   }

   bool
   TransparentTransmissionBuffer::Flush(bool bForce)
   {
      bool dataProcessed = false;

      if (!GetRequiresFlush() && !bForce)
         return dataProcessed;

      if (buffer_->GetSize() > MAX_LINE_LENGTH)
      {
         // Something fishy is going on. We've received over MAX_LINE_LENGTH
         // characters on a single line with no new line character. This should
         // never happen in normal email communication so let's assume someone
         // is trying to attack us.
         cancel_transmission_ = true;
         cancel_message_ = "Too long line was received. Transmission aborted.";
         bForce = true;
      }

      // Locate last \n
      const char *pBuffer = buffer_->GetCharBuffer();
      int bufferSize = buffer_->GetSize();
      
      /*
         RFC rfc2821
         text line
            The maximum total length of a text line including the <CRLF> is
            1000 characters (not counting the leading dot duplicated for
            transparency).  This number may be increased by the use of SMTP
            Service Extensions.
      */

      int maxLineLength = MAX_LINE_LENGTH;

      // Start in the end and move 'back' MAX_LINE_LENGTH characters.
      int searchEndPos = max(bufferSize - maxLineLength, 0);
      for (int i = bufferSize - 1; i >= searchEndPos; i--)
      {
         char s = pBuffer[i];

         // If we found a newline, send anything up until that.
         // If we're forcing a send, send all we got
         // If we found no newline in the stream, the message is malformed according to RFC2821 (max 1000 chars per line). 
         //    Send all we got anyway. 

         if (s == '\n' || bForce)
         {
            last_send_ended_with_newline_ = s == '\n';

            // Copy the data up including this position
            int iCopySize = i+1;

            std::shared_ptr<ByteBuffer> pOutBuffer = std::shared_ptr<ByteBuffer>(new ByteBuffer);
            pOutBuffer->Add(buffer_->GetBuffer(), iCopySize);

            // Remove it from the old buffer
            int iRemaining = buffer_->GetSize() - iCopySize;
            buffer_->Empty(iRemaining);

            // Parse this buffer and add it to file/socket
            if (is_sending_)
               InsertTransmissionPeriod_(pOutBuffer);
            else
               RemoveTransmissionPeriod_(pOutBuffer);

            // The parsed buffer can now be sent.
            if (is_sending_)
            {
               if (std::shared_ptr<TCPConnection> connection = tcp_connection_.lock())
               {
                  connection->EnqueueWrite(pOutBuffer);
               }
               
            }
            else 
            {
               SaveToFile_(pOutBuffer);
            }

            dataProcessed = true;

            break;
         }
      }

      if (transmission_ended_ && file_.IsOpen())
      {
         file_.Close();
      }

      return dataProcessed;
   }

   bool 
   TransparentTransmissionBuffer::SaveToFile_(std::shared_ptr<ByteBuffer> pBuffer)
   {
      if (max_size_kb_ > 0 && (data_sent_ / 1024) > max_size_kb_)
      {
         // we've reached the max size. don't save more data.
         return false;
      }

      if (!cancel_transmission_)
      {
         DWORD dwNoOfBytesWritten = 0;
         bool bResult = file_.Write(pBuffer, dwNoOfBytesWritten);
      }
 
      return true;
   }

   void 
   TransparentTransmissionBuffer::InsertTransmissionPeriod_(std::shared_ptr<ByteBuffer> pBuffer)
   {
      // All . which are placed as the first character on a new
      // line should be replaced with ..
      
      // Allocate maximum required length for the out buffer.
      char *pInBuffer = (char*) pBuffer->GetCharBuffer();

      char *pOutBuffer = new char[pBuffer->GetSize() * 2];
      char *pOutBufferStart = pOutBuffer;

      int iInBufferSize = pBuffer->GetSize();
     
      for (int i = 0; i < iInBufferSize; i++)
      {
         char c = pInBuffer[i];
         if (c == '.')
         {
            if (i == 0)
            {
               *pOutBuffer = '.';
               pOutBuffer++;
            }
            else if (i > 2 && pInBuffer[i-1] == '\n')
            {
               *pOutBuffer = '.';
               pOutBuffer++;
            }
         }

         // Add the character
         *pOutBuffer = c;
         pOutBuffer++;
      }

      // Clear the buffer and insert the new data
      int iOutBufferLen = pOutBuffer - pOutBufferStart;
      pBuffer->Empty();
      pBuffer->Add((BYTE*) pOutBufferStart, iOutBufferLen);

      // Free memory for the old buffer
      delete [] pOutBufferStart;
   }

   void
   TransparentTransmissionBuffer::RemoveTransmissionPeriod_(std::shared_ptr<ByteBuffer> pBuffer)
   {
      // Allocate maximum required length for the out buffer.
      char *pInBuffer = (char*) pBuffer->GetCharBuffer();

      char *pOutBuffer = new char[pBuffer->GetSize()];
      char *pOutBufferStart = pOutBuffer;

      int iInBufferSize = pBuffer->GetSize();

      for (int i = 0; i < iInBufferSize; i++)
      {
         char c = pInBuffer[i];
         if (c == '.')
         {
            if (i == 0)
               continue;
            else if (i > 2 && pInBuffer[i-1] == '\n')
               continue;
         }

         // Add the character to the out buffer
         *pOutBuffer = c;
         pOutBuffer++;

      }

      // Clear the buffer and insert the new data
      int iOutBufferLen = pOutBuffer - pOutBufferStart;
      pBuffer->Empty();
      pBuffer->Add((BYTE*) pOutBufferStart, iOutBufferLen);

      // Free memory for the old buffer
      delete [] pOutBufferStart;
   }

}