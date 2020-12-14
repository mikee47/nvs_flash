// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <unordered_map>
#include "Item.hpp"
#include "Page.hpp"
#include "PageManager.hpp"
#include "Handle.hpp"

//extern void dumpBytes(const uint8_t* data, size_t count);

namespace nvs
{
/**
 * @brief Mode of opening the non-volatile storage
 */
enum class OpenMode {
	ReadOnly,
	ReadWrite,
};

class Storage : public intrusive_list_node<Storage>
{
	enum class State {
		INVALID,
		ACTIVE,
	};

	struct NamespaceEntry : public intrusive_list_node<NamespaceEntry> {
	public:
		char mName[Item::MAX_KEY_LENGTH + 1];
		uint8_t mIndex;
	};

	typedef intrusive_list<NamespaceEntry> TNamespaces;

	struct UsedPageNode : public intrusive_list_node<UsedPageNode> {
	public:
		Page* mPage;
	};

	typedef intrusive_list<UsedPageNode> TUsedPageList;

	struct BlobIndexNode : public intrusive_list_node<BlobIndexNode> {
	public:
		char key[Item::MAX_KEY_LENGTH + 1];
		uint8_t nsIndex;
		uint8_t chunkCount;
		VerOffset chunkStart;
	};

	typedef intrusive_list<BlobIndexNode> TBlobIndexList;

	class ItemIterator : public Item
	{
	public:
		ItemIterator(Storage& storage, const char* ns_name, ItemType itemType);

		void reset();

		bool next();

		explicit operator bool() const
		{
			return page && err == ESP_OK;
		}

		String ns_name() const;

	private:
		Storage& storage;
		ItemType itemType;
		uint8_t nsIndex{Page::NS_ANY};
		size_t entryIndex{0};
		intrusive_list<nvs::Page>::iterator page;
		err_t err{ESP_OK};
	};

public:
	~Storage();

	Storage(Partition& partition) : mPartition(partition)
	{
	}

	esp_err_t init(uint32_t baseSector, uint32_t sectorCount);

	bool isValid() const;

	esp_err_t createOrOpenNamespace(const char* nsName, bool canCreate, uint8_t& nsIndex);

	HandlePtr open_handle(const char* ns_name, OpenMode open_mode);

	esp_err_t writeItem(uint8_t nsIndex, ItemType datatype, const char* key, const void* data, size_t dataSize);

	esp_err_t readItem(uint8_t nsIndex, ItemType datatype, const char* key, void* data, size_t dataSize);

	esp_err_t getItemDataSize(uint8_t nsIndex, ItemType datatype, const char* key, size_t& dataSize);

	esp_err_t eraseItem(uint8_t nsIndex, ItemType datatype, const char* key);

	template <typename T> esp_err_t writeItem(uint8_t nsIndex, const char* key, const T& value)
	{
		return writeItem(nsIndex, itemTypeOf(value), key, &value, sizeof(value));
	}

	template <typename T> esp_err_t readItem(uint8_t nsIndex, const char* key, T& value)
	{
		return readItem(nsIndex, itemTypeOf(value), key, &value, sizeof(value));
	}

	esp_err_t eraseItem(uint8_t nsIndex, const char* key)
	{
		return eraseItem(nsIndex, ItemType::ANY, key);
	}

	esp_err_t eraseNamespace(uint8_t nsIndex);

	const ::Storage::Partition& partition() const
	{
		return mPartition;
	}

	uint32_t getBaseSector()
	{
		return mPageManager.getBaseSector();
	}

	esp_err_t writeMultiPageBlob(uint8_t nsIndex, const char* key, const void* data, size_t dataSize,
								 VerOffset chunkStart);

	esp_err_t readMultiPageBlob(uint8_t nsIndex, const char* key, void* data, size_t dataSize);

	esp_err_t cmpMultiPageBlob(uint8_t nsIndex, const char* key, const void* data, size_t dataSize);

	esp_err_t eraseMultiPageBlob(uint8_t nsIndex, const char* key, VerOffset chunkStart = VerOffset::VER_ANY);

	void debugDump();

	void debugCheck();

	esp_err_t fillStats(nvs_stats_t& nvsStats);

	esp_err_t calcEntriesInNamespace(uint8_t nsIndex, size_t& usedEntries);

	ItemIterator findEntry(const char* namespace_name = nullptr, ItemType itemType = ItemType::ANY)
	{
		return ItemIterator(*this, namespace_name, itemType);
	}

	esp_err_t lastError() const
	{
		return mLastError;
	}

	void invalidate_handles();

private:
	friend Handle;
	friend ItemIterator;

	Page& getCurrentPage()
	{
		return mPageManager.back();
	}

	void clearNamespaces();

	esp_err_t populateBlobIndices(TBlobIndexList&);

	void eraseOrphanDataBlobs(TBlobIndexList&);

	esp_err_t findItem(uint8_t nsIndex, ItemType datatype, const char* key, Page*& page, Item& item,
					   uint8_t chunkIdx = Page::CHUNK_ANY, VerOffset chunkStart = VerOffset::VER_ANY);

	// Called from Handle destructor
	bool close_handle(Handle* handle);

	Partition& mPartition;
	intrusive_list<Handle> handle_list;
	size_t mPageCount;
	PageManager mPageManager;
	TNamespaces mNamespaces;
	CompressedEnumTable<bool, 1, 256> mNamespaceUsage;
	State mState{State::INVALID};
	esp_err_t mLastError{ESP_OK};
};

} // namespace nvs

static constexpr nvs::OpenMode NVS_READONLY{nvs::OpenMode::ReadOnly};
static constexpr nvs::OpenMode NVS_READWRITE{nvs::OpenMode::ReadWrite};
