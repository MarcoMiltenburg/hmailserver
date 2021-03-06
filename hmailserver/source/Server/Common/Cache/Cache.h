// Copyright (c) 2010 Martin Knafve / hMailServer.com.  
// http://www.hmailserver.com

namespace HM
{
   template <class T, class P>
   class Cache : public Singleton<Cache<T,P>>
   {
   public:
      Cache();

      std::shared_ptr<const T> GetObject(const String &sName);
      // Retrieves an object using the object name.

      std::shared_ptr<const T> GetObject(__int64 iID);
      // Retrieves an object using the ID

      void RemoveObject(std::shared_ptr<T> pObject);
      void RemoveObject(const String &sName);
      void RemoveObject(__int64 iID);

      void SetTTL(int iNewVal);
      int GetHitRate();
      void SetEnabled(bool bEnabled);
      void Clear();

   private:

      bool GetObjectIsWithinTTL_(std::shared_ptr<T> pObject);
      void AddToCache_(std::shared_ptr<T> pObject);

      int no_of_misses_;
      int no_of_hits_;
      int ttl_;
      bool enabled_;


      // Properties used to determine how long objects 
      // should be stored and the current statistics

      boost::recursive_mutex _mutex;
      // All access to the container is restricted by
      // a critical section
      
      std::map<String, std::shared_ptr<T> > objects_;
      // All the objects in the cache
   };

   template <class T, class P> 
   Cache<T,P>::Cache()
   {
      no_of_misses_ = 0;
      no_of_hits_ = 0;
      ttl_ = 0;
      enabled_ = false;
   }

   template <class T, class P> 
   void
   Cache<T,P>::Clear()
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      objects_.clear();
      no_of_misses_ = 0;
      no_of_hits_ = 0;
   }

   template <class T, class P> 
   void
   Cache<T,P>::SetTTL(int iNewVal)
   {
      ttl_ = iNewVal;

      no_of_misses_ = 0;
      no_of_hits_ = 0;
   }


   template <class T, class P> 
   void
   Cache<T,P>::SetEnabled(bool bEnabled)
   {
      enabled_ = bEnabled;

      if (!enabled_)
         Clear();
   }


   template <class T, class P> 
   int
   Cache<T,P>::GetHitRate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      if (no_of_hits_ == 0)
         return 0;

      int iHitRate = (int) (((float) no_of_hits_ / (float) (no_of_hits_ + no_of_misses_)) * 100);

      return iHitRate;
   }

   template <class T, class P> 
   void 
   Cache<T,P>::RemoveObject(std::shared_ptr<T> pObject)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      auto iterObject = objects_.find(pObject->GetName());
   
      if (iterObject != objects_.end())
         objects_.erase(iterObject);

   }

   template <class T, class P> 
   void 
   Cache<T,P>::RemoveObject(const String &sName)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      auto iterObject = objects_.find(sName);

      if (iterObject != objects_.end())
         objects_.erase(iterObject);

   }

   template <class T, class P> 
   void 
   Cache<T,P>::RemoveObject(__int64 iID)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      // Find the domain using the ID
      auto iterObject = objects_.begin();
      auto iterEnd = objects_.end();

      for (; iterObject != iterEnd; iterObject++)
      {
         std::shared_ptr<T> pObject = (*iterObject).second;

         if (pObject->GetID() == iID)
         {
            objects_.erase(iterObject);
            return;
         }

      }
   }

   template <class T, class P> 
   std::shared_ptr<const T> 
   Cache<T,P>::GetObject(const String &sName)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      if (enabled_)
      {
         auto iterObject = objects_.find(sName);

         if (iterObject != objects_.end())
         {
            std::shared_ptr<T> pObject = (*iterObject).second;

            if (GetObjectIsWithinTTL_(pObject))
               return pObject;
         
            // Object has passed TTL
            objects_.erase(iterObject);
         }
      }

      // Load the object
      std::shared_ptr<T> pRetObject = std::shared_ptr<T>(new T);
      
      if (!P::ReadObject(pRetObject, sName))
      {
         std::shared_ptr<T> pEmpty;
         return pEmpty;
      }

      if (enabled_)
         AddToCache_(pRetObject);

      return pRetObject;
   }

   template <class T, class P> 
   std::shared_ptr<const T> 
   Cache<T,P>::GetObject(__int64 iID)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      if (enabled_)
      {
         // Find the domain using the ID
         auto iterObject = objects_.begin();
         auto iterEnd = objects_.end();

         for (; iterObject != iterEnd; iterObject++)
         {
            std::shared_ptr<T> pObject = (*iterObject).second;

            if (pObject->GetID() == iID)
            {
               if (GetObjectIsWithinTTL_(pObject))
                  return pObject;

               objects_.erase(iterObject);

               break;
            }
         }
      }

      // Load the object
      std::shared_ptr<T> pRetObject = std::shared_ptr<T>(new T);
      if (!P::ReadObject(pRetObject, iID))
      {
         std::shared_ptr<T> pEmpty;
         return pEmpty;
      }

      if (enabled_)
         AddToCache_(pRetObject);
   
      return pRetObject;
   }

   template <class T, class P> 
   void 
   Cache<T,P>::AddToCache_(std::shared_ptr<T> pObject)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      // Object must be saved before it can be cached.
#ifdef DEBUG
      if (pObject->GetID() == 0)
      {
         assert(0);
      }
#endif

      no_of_misses_++;
      objects_[pObject->GetName()] = pObject;
   }

   template <class T, class P> 
   bool 
   Cache<T,P>::GetObjectIsWithinTTL_(std::shared_ptr<T> pObject)
   {
      if (pObject)
      {
         if (pObject->Seconds() < ttl_)
         {
            // A fresh object was found in the cache.
            no_of_hits_++;
            return true;
         }
      }

      return false;
   }
}
