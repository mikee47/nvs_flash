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
#include "include/Nvs/Storage.hpp"
#include "include/Nvs/Handle.hpp"

#ifdef ARCH_HOST
#include <map>
#include <sstream>
#endif

namespace nvs
{
bool Storage::populateBlobIndices(TBlobIndexList& blobIdxList)
{
	for(auto it = mPageManager.begin(); it != mPageManager.end(); ++it) {
		Page& p = *it;
		size_t itemIndex = 0;
		Item item;

		/* If the power went off just after writing a blob index, the duplicate detection
         * logic in pagemanager will remove the earlier index. So we should never find a
         * duplicate index at this point */

		while(p.findItem(Page::NS_ANY, ItemType::BLOB_IDX, nullptr, itemIndex, item) == ESP_OK) {
			BlobIndexNode* entry = new(std::nothrow) BlobIndexNode;

			if(!entry) {
				mLastError = ESP_ERR_NO_MEM;
				return false;
			}

			item.getKey(entry->key, sizeof(entry->key));
			entry->nsIndex = item.nsIndex;
			entry->chunkStart = item.blobIndex.chunkStart;
			entry->chunkCount = item.blobIndex.chunkCount;

			blobIdxList.push_back(entry);
			itemIndex += item.span;
		}
	}

	mLastError = ESP_OK;
	return true;
}

void Storage::eraseOrphanDataBlobs(TBlobIndexList& blobIdxList)
{
	for(auto it = mPageManager.begin(); it != mPageManager.end(); ++it) {
		Page& p = *it;
		size_t itemIndex = 0;
		Item item;
		/* Chunks with same <ns,key> and with chunkIndex in the following ranges
         * belong to same family.
         * 1) VER_0_OFFSET <= chunkIndex < VER_1_OFFSET-1 => Version0 chunks
         * 2) VER_1_OFFSET <= chunkIndex < VER_ANY => Version1 chunks
         */
		while(p.findItem(Page::NS_ANY, ItemType::BLOB_DATA, nullptr, itemIndex, item) == ESP_OK) {
			auto iter = std::find_if(blobIdxList.begin(), blobIdxList.end(), [=](const BlobIndexNode& e) -> bool {
				return (strncmp(item.key, e.key, sizeof(e.key) - 1) == 0) && (item.nsIndex == e.nsIndex) &&
					   (item.chunkIndex >= uint8_t(e.chunkStart)) &&
					   (item.chunkIndex < uint8_t(e.chunkStart) + e.chunkCount);
			});
			if(iter == std::end(blobIdxList)) {
				p.eraseItem(item.nsIndex, item.datatype, item.key, item.chunkIndex);
			}
			itemIndex += item.span;
		}
	}
}

bool Storage::init()
{
	if(mHandleCount != 0) {
		debug_e("Handles in use, cannot init");
		assert(false);
		mLastError = ESP_ERR_NVS_INVALID_STATE;
		return false;
	}

	mLastError = mPageManager.load(*mPartition);
	if(mLastError != ESP_OK) {
		mState = State::INVALID;
		return false;
	}

	// load namespaces list
	mNamespaces.clearAndFreeNodes();
	std::fill_n(mNamespaceUsage.data(), mNamespaceUsage.byteSize() / 4, 0);
	for(auto it = mPageManager.begin(); it != mPageManager.end(); ++it) {
		Page& p = *it;
		size_t itemIndex = 0;
		Item item;
		while(p.findItem(Page::NS_INDEX, ItemType::U8, nullptr, itemIndex, item) == ESP_OK) {
			NamespaceEntry* entry = new(std::nothrow) NamespaceEntry;

			if(!entry) {
				mState = State::INVALID;
				mLastError = ESP_ERR_NO_MEM;
				return false;
			}

			item.getKey(entry->mName, sizeof(entry->mName));
			item.getValue(entry->mIndex);
			mNamespaces.push_back(entry);
			mNamespaceUsage.set(entry->mIndex, true);
			itemIndex += item.span;
		}
	}
	mNamespaceUsage.set(0, true);
	mNamespaceUsage.set(255, true);
	mState = State::ACTIVE;

	// Populate list of multi-page index entries.
	TBlobIndexList blobIdxList;
	if(!populateBlobIndices(blobIdxList)) {
		mState = State::INVALID;
		return false;
	}

	// Remove the entries for which there is no parent multi-page index.
	eraseOrphanDataBlobs(blobIdxList);

	// Purge the blob index list
	blobIdxList.clearAndFreeNodes();

#ifdef ARCH_HOST
	debugCheck();
#endif

	mLastError = ESP_OK;
	return true;
}

bool Storage::findItem(uint8_t nsIndex, ItemType datatype, const char* key, Page*& page, Item& item, uint8_t chunkIdx,
					   VerOffset chunkStart)
{
	for(auto it = std::begin(mPageManager); it != std::end(mPageManager); ++it) {
		size_t itemIndex = 0;
		mLastError = it->findItem(nsIndex, datatype, key, itemIndex, item, chunkIdx, chunkStart);
		if(mLastError == ESP_OK) {
			page = it;
			return true;
		}
	}

	mLastError = ESP_ERR_NVS_NOT_FOUND;
	return false;
}

bool Storage::writeMultiPageBlob(uint8_t nsIndex, const char* key, const void* data, size_t dataSize,
								 VerOffset chunkStart)
{
	/* Check how much maximum data can be accommodated**/
	uint32_t max_pages = std::min(mPageManager.getPageCount() - 1, (Page::CHUNK_ANY - 1) / 2U);

	if(dataSize > max_pages * Page::CHUNK_MAX_SIZE) {
		mLastError = ESP_ERR_NVS_VALUE_TOO_LONG;
		return false;
	}

	uint8_t chunkCount{0};
	intrusive_list<UsedPageNode> usedPages;
	size_t remainingSize = dataSize;
	size_t offset{0};

	do {
		Page& page = getCurrentPage();
		size_t tailroom = page.getVarDataTailroom();
		size_t chunkSize = 0;
		if(!chunkCount && tailroom < dataSize && tailroom < Page::CHUNK_MAX_SIZE / 10) {
			/** This is the first chunk and tailroom is too small ***/
			if(page.state() != Page::PageState::FULL) {
				mLastError = page.markFull();
				if(mLastError != ESP_OK) {
					return false;
				}
			}
			mLastError = mPageManager.requestNewPage();
			if(mLastError != ESP_OK) {
				return false;
			} else if(getCurrentPage().getVarDataTailroom() == tailroom) {
				/* We got the same page or we are not improving.*/
				mLastError = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
				return false;
			} else {
				continue;
			}
		} else if(!tailroom) {
			mLastError = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
			break;
		}

		/* Split the blob into two and store the chunk of available size onto the current page */
		assert(tailroom != 0);
		chunkSize = (remainingSize > tailroom) ? tailroom : remainingSize;
		remainingSize -= chunkSize;

		mLastError = page.writeItem(nsIndex, ItemType::BLOB_DATA, key, static_cast<const uint8_t*>(data) + offset,
									chunkSize, uint8_t(chunkStart) + chunkCount);
		chunkCount++;
		assert(mLastError != ESP_ERR_NVS_PAGE_FULL);
		if(mLastError != ESP_OK) {
			break;
		}

		auto node = new(std::nothrow) UsedPageNode;
		if(node == nullptr) {
			mLastError = ESP_ERR_NO_MEM;
			break;
		}
		node->mPage = &page;
		usedPages.push_back(node);
		if(remainingSize != 0 || (tailroom - chunkSize) < Page::ENTRY_SIZE) {
			if(page.state() != Page::PageState::FULL) {
				mLastError = page.markFull();
				if(mLastError != ESP_OK) {
					break;
				}
			}
			mLastError = mPageManager.requestNewPage();
			if(mLastError != ESP_OK) {
				break;
			}
		}

		offset += chunkSize;
		if(remainingSize == 0) {
			/* All pages are stored. Now store the index.*/
			Item item;
			std::fill_n(item.data, sizeof(item.data), 0xff);
			item.blobIndex.dataSize = dataSize;
			item.blobIndex.chunkCount = chunkCount;
			item.blobIndex.chunkStart = chunkStart;

			mLastError = getCurrentPage().writeItem(nsIndex, ItemType::BLOB_IDX, key, item.data, sizeof(item.data));
			assert(mLastError != ESP_ERR_NVS_PAGE_FULL);
			break;
		}
	} while(true);

	if(mLastError != ESP_OK) {
		/* Anything failed, then we should erase all the written chunks*/
		int ii = 0;
		for(auto it = std::begin(usedPages); it != std::end(usedPages); it++) {
			it->mPage->eraseItem(nsIndex, ItemType::BLOB_DATA, key, ii++);
		}
	}
	usedPages.clearAndFreeNodes();

	return mLastError == ESP_OK;
}

bool Storage::writeItem(uint8_t nsIndex, ItemType datatype, const char* key, const void* data, size_t dataSize)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	Page* findPage = nullptr;
	Item item;

	if(!findItem(nsIndex, (datatype == ItemType::BLOB) ? ItemType::BLOB_IDX : datatype, key, findPage, item)) {
		if(mLastError != ESP_ERR_NVS_NOT_FOUND) {
			return false;
		}
	}

	if(datatype == ItemType::BLOB) {
		VerOffset prevStart, nextStart;
		prevStart = nextStart = VerOffset::VER_0_OFFSET;
		if(findPage) {
			// Do a sanity check that the item in question is actually being modified.
			// If it isn't, it is cheaper to purposefully not write out new data.
			// since it may invoke an erasure of flash.
			if(cmpMultiPageBlob(nsIndex, key, data, dataSize)) {
				return true;
			}

			if(findPage->state() == Page::PageState::UNINITIALIZED || findPage->state() == Page::PageState::INVALID) {
				ESP_ERROR_CHECK(findItem(nsIndex, datatype, key, findPage, item));
			}
			/* Get the version of the previous index with same <ns,key> */
			prevStart = item.blobIndex.chunkStart;
			assert(prevStart == VerOffset::VER_0_OFFSET || prevStart == VerOffset::VER_1_OFFSET);

			/* Toggle the version by changing the offset */
			nextStart = (prevStart == VerOffset::VER_1_OFFSET) ? VerOffset::VER_0_OFFSET : VerOffset::VER_1_OFFSET;
		}

		/* Write the blob with new version*/
		if(!writeMultiPageBlob(nsIndex, key, data, dataSize, nextStart)) {
			if(mLastError == ESP_ERR_NVS_PAGE_FULL) {
				mLastError = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
			}
			return false;
		}

		if(findPage) {
			/* Erase the blob with earlier version*/
			if(!eraseMultiPageBlob(nsIndex, key, prevStart)) {
				return false;
			}

			findPage = nullptr;
		} else {
			/* Support for earlier versions where BLOBS were stored without index */
			if(!findItem(nsIndex, datatype, key, findPage, item)) {
				if(mLastError != ESP_ERR_NVS_NOT_FOUND) {
					return false;
				}
			}
		}
	} else {
		// Do a sanity check that the item in question is actually being modified.
		// If it isn't, it is cheaper to purposefully not write out new data.
		// since it may invoke an erasure of flash.
		if(findPage != nullptr && findPage->cmpItem(nsIndex, datatype, key, data, dataSize) == ESP_OK) {
			mLastError = ESP_OK;
			return true;
		}

		Page& page = getCurrentPage();
		mLastError = page.writeItem(nsIndex, datatype, key, data, dataSize);
		if(mLastError == ESP_ERR_NVS_PAGE_FULL) {
			if(page.state() != Page::PageState::FULL) {
				mLastError = page.markFull();
				if(mLastError != ESP_OK) {
					return false;
				}
			}
			mLastError = mPageManager.requestNewPage();
			if(mLastError != ESP_OK) {
				return false;
			}

			mLastError = getCurrentPage().writeItem(nsIndex, datatype, key, data, dataSize);
			if(mLastError == ESP_ERR_NVS_PAGE_FULL) {
				mLastError = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
				return false;
			}
			if(mLastError != ESP_OK) {
				return false;
			}
		} else if(mLastError != ESP_OK) {
			return false;
		}
	}

	if(findPage) {
		if(findPage->state() == Page::PageState::UNINITIALIZED || findPage->state() == Page::PageState::INVALID) {
			ESP_ERROR_CHECK(findItem(nsIndex, datatype, key, findPage, item));
		}
		mLastError = findPage->eraseItem(nsIndex, datatype, key);
		if(mLastError != ESP_OK) {
			return false;
		}
	}
#ifdef ARCH_HOST
	debugCheck();
#endif

	mLastError = ESP_OK;
	return true;
}

bool Storage::createOrOpenNamespace(const char* nsName, bool canCreate, uint8_t& nsIndex)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	auto it = std::find(mNamespaces.begin(), mNamespaces.end(), nsName);
	if(it == std::end(mNamespaces)) {
		if(!canCreate) {
			mLastError = ESP_ERR_NVS_NOT_FOUND;
			return false;
		}

		uint8_t ns;
		for(ns = 1; ns < 255; ++ns) {
			if(mNamespaceUsage.get(ns) == false) {
				break;
			}
		}

		if(ns == 255) {
			mLastError = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
			return false;
		}

		NamespaceEntry* entry = new(std::nothrow) NamespaceEntry;
		if(entry == nullptr) {
			mLastError = ESP_ERR_NO_MEM;
			return false;
		}

		if(!writeItem(Page::NS_INDEX, ItemType::U8, nsName, &ns, sizeof(ns))) {
			return false;
		}

		mNamespaceUsage.set(ns, true);
		nsIndex = ns;

		entry->mIndex = ns;
		strncpy(entry->mName, nsName, sizeof(entry->mName) - 1);
		entry->mName[sizeof(entry->mName) - 1] = '\0';
		mNamespaces.push_back(entry);

	} else {
		nsIndex = it->mIndex;
	}

	mLastError = ESP_OK;
	return true;
}

bool Storage::readMultiPageBlob(uint8_t nsIndex, const char* key, void* data, size_t dataSize)
{
	Item item;
	Page* findPage{nullptr};

	/* First read the blob index */
	if(!findItem(nsIndex, ItemType::BLOB_IDX, key, findPage, item)) {
		return false;
	}

	uint8_t chunkCount = item.blobIndex.chunkCount;
	VerOffset chunkStart = item.blobIndex.chunkStart;
	size_t readSize = item.blobIndex.dataSize;
	size_t offset = 0;

	assert(dataSize == readSize);

	/* Now read corresponding chunks */
	for(uint8_t chunkNum = 0; chunkNum < chunkCount; chunkNum++) {
		if(!findItem(nsIndex, ItemType::BLOB_DATA, key, findPage, item, uint8_t(chunkStart) + chunkNum)) {
			if(mLastError == ESP_ERR_NVS_NOT_FOUND) {
				break;
			}
			return false;
		}
		mLastError = findPage->readItem(nsIndex, ItemType::BLOB_DATA, key, static_cast<uint8_t*>(data) + offset,
										item.varLength.dataSize, uint8_t(chunkStart) + chunkNum);
		if(mLastError != ESP_OK) {
			return false;
		}
		assert(uint8_t(chunkStart) + chunkNum == item.chunkIndex);
		offset += item.varLength.dataSize;
	}

	if(mLastError == ESP_OK) {
		assert(offset == dataSize);
		return true;
	}

	if(mLastError == ESP_ERR_NVS_NOT_FOUND) {
		eraseMultiPageBlob(nsIndex, key); // cleanup if a chunk is not found
	}

	mLastError = ESP_ERR_NVS_NOT_FOUND;
	return false;
}

bool Storage::cmpMultiPageBlob(uint8_t nsIndex, const char* key, const void* data, size_t dataSize)
{
	Item item;
	Page* findPage = nullptr;

	/* First read the blob index */
	if(!findItem(nsIndex, ItemType::BLOB_IDX, key, findPage, item)) {
		return false;
	}

	uint8_t chunkCount = item.blobIndex.chunkCount;
	VerOffset chunkStart = item.blobIndex.chunkStart;
	size_t readSize = item.blobIndex.dataSize;
	size_t offset = 0;

	if(dataSize != readSize) {
		mLastError = ESP_ERR_NVS_CONTENT_DIFFERS;
		return false;
	}

	/* Now read corresponding chunks */
	for(uint8_t chunkNum = 0; chunkNum < chunkCount; chunkNum++) {
		if(!findItem(nsIndex, ItemType::BLOB_DATA, key, findPage, item, uint8_t(chunkStart) + chunkNum)) {
			if(mLastError == ESP_ERR_NVS_NOT_FOUND) {
				break;
			}
			return false;
		}
		mLastError = findPage->cmpItem(nsIndex, ItemType::BLOB_DATA, key, static_cast<const uint8_t*>(data) + offset,
									   item.varLength.dataSize, uint8_t(chunkStart) + chunkNum);
		if(mLastError != ESP_OK) {
			return false;
		}
		assert(uint8_t(chunkStart) + chunkNum == item.chunkIndex);
		offset += item.varLength.dataSize;
	}

	if(mLastError == ESP_OK) {
		assert(offset == dataSize);
		return true;
	}

	return false;
}

bool Storage::readItem(uint8_t nsIndex, ItemType datatype, const char* key, void* data, size_t dataSize)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	Item item;
	Page* findPage = nullptr;
	if(datatype == ItemType::BLOB) {
		if(readMultiPageBlob(nsIndex, key, data, dataSize)) {
			return true;
		}
		if(mLastError != ESP_ERR_NVS_NOT_FOUND) {
			return false;
		} // else check if the blob is stored with earlier version format without index
	}

	if(!findItem(nsIndex, datatype, key, findPage, item)) {
		return false;
	}

	mLastError = findPage->readItem(nsIndex, datatype, key, data, dataSize);
	return mLastError == ESP_OK;
}

bool Storage::eraseMultiPageBlob(uint8_t nsIndex, const char* key, VerOffset chunkStart)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	Item item;
	Page* findPage = nullptr;

	if(!findItem(nsIndex, ItemType::BLOB_IDX, key, findPage, item, Page::CHUNK_ANY, chunkStart)) {
		return false;
	}

	/* Erase the index first and make children blobs orphan*/
	mLastError = findPage->eraseItem(nsIndex, ItemType::BLOB_IDX, key, Page::CHUNK_ANY, chunkStart);
	if(mLastError != ESP_OK) {
		return false;
	}

	uint8_t chunkCount = item.blobIndex.chunkCount;

	if(chunkStart == VerOffset::VER_ANY) {
		chunkStart = item.blobIndex.chunkStart;
	} else {
		assert(chunkStart == item.blobIndex.chunkStart);
	}

	/* Now erase corresponding chunks*/
	for(uint8_t chunkNum = 0; chunkNum < chunkCount; chunkNum++) {
		if(!findItem(nsIndex, ItemType::BLOB_DATA, key, findPage, item, uint8_t(chunkStart) + chunkNum)) {
			if(mLastError != ESP_ERR_NVS_NOT_FOUND) {
				return false;
			}
			continue; // Keep erasing other chunks
		}

		mLastError = findPage->eraseItem(nsIndex, ItemType::BLOB_DATA, key, uint8_t(chunkStart) + chunkNum);
		if(mLastError != ESP_OK) {
			return false;
		}
	}

	mLastError = ESP_OK;
	return true;
}

bool Storage::eraseItem(uint8_t nsIndex, ItemType datatype, const char* key)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	if(datatype == ItemType::BLOB) {
		return eraseMultiPageBlob(nsIndex, key);
	}

	Item item;
	Page* findPage = nullptr;
	if(!findItem(nsIndex, datatype, key, findPage, item)) {
		return false;
	}

	if(item.datatype == ItemType::BLOB_DATA || item.datatype == ItemType::BLOB_IDX) {
		return eraseMultiPageBlob(nsIndex, key);
	}

	mLastError = findPage->eraseItem(nsIndex, datatype, key);
	return mLastError == ESP_OK;
}

bool Storage::eraseNamespace(uint8_t nsIndex)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	for(auto it = std::begin(mPageManager); it != std::end(mPageManager); ++it) {
		while(true) {
			auto err = it->eraseItem(nsIndex, ItemType::ANY, nullptr);
			if(err == ESP_ERR_NVS_NOT_FOUND) {
				break;
			}

			if(err != ESP_OK) {
				mLastError = err;
				return false;
			}
		}
	}

	mLastError = ESP_OK;
	return true;
}

bool Storage::getItemDataSize(uint8_t nsIndex, ItemType datatype, const char* key, size_t& dataSize)
{
	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	Item item;
	Page* findPage = nullptr;
	if(findItem(nsIndex, datatype, key, findPage, item)) {
		dataSize = item.varLength.dataSize;
		return true;
	}

	if(datatype != ItemType::BLOB) {
		return false;
	}

	if(findItem(nsIndex, ItemType::BLOB_IDX, key, findPage, item)) {
		dataSize = item.blobIndex.dataSize;
		return true;
	}

	return false;
}

void Storage::debugDump()
{
	for(auto p = mPageManager.begin(); p != mPageManager.end(); ++p) {
		p->debugDump();
	}
}

#ifdef ARCH_HOST
void Storage::debugCheck()
{
	std::map<std::string, Page*> keys;

	for(auto p = mPageManager.begin(); p != mPageManager.end(); ++p) {
		size_t itemIndex = 0;
		size_t usedCount = 0;
		Item item;
		while(p->findItem(Page::NS_ANY, ItemType::ANY, nullptr, itemIndex, item) == ESP_OK) {
			std::stringstream keyrepr;
			keyrepr << static_cast<unsigned>(item.nsIndex) << "_" << static_cast<unsigned>(item.datatype) << "_"
					<< item.key << "_" << static_cast<unsigned>(item.chunkIndex);
			std::string keystr = keyrepr.str();
			if(keys.find(keystr) != std::end(keys)) {
				printf("Duplicate key: %s\n", keystr.c_str());
				debugDump();
				assert(0);
			}
			keys.insert(std::make_pair(keystr, static_cast<Page*>(p)));
			itemIndex += item.span;
			usedCount += item.span;
		}
		assert(usedCount == p->getUsedEntryCount());
	}
}
#endif //ESP_PLATFORM

bool Storage::fillStats(nvs_stats_t& nvsStats)
{
	nvsStats.namespace_count = mNamespaces.size();
	mLastError = mPageManager.fillStats(nvsStats);
	return mLastError == ESP_OK;
}

bool Storage::calcEntriesInNamespace(uint8_t nsIndex, size_t& usedEntries)
{
	usedEntries = 0;

	if(mState != State::ACTIVE) {
		mLastError = ESP_ERR_NVS_NOT_INITIALIZED;
		return false;
	}

	for(auto it = std::begin(mPageManager); it != std::end(mPageManager); ++it) {
		size_t itemIndex = 0;
		Item item;
		while(true) {
			auto err = it->findItem(nsIndex, ItemType::ANY, nullptr, itemIndex, item);
			if(err == ESP_ERR_NVS_NOT_FOUND) {
				break;
			}
			if(err != ESP_OK) {
				mLastError = err;
				return false;
			}

			usedEntries += item.span;
			itemIndex += item.span;
			if(itemIndex >= it->ENTRY_COUNT) {
				break;
			}
		}
	}

	mLastError = ESP_OK;
	return true;
}

Storage::ItemIterator::ItemIterator(Storage& storage, const char* ns_name, ItemType itemType)
	: storage(storage), itemType(itemType)
{
	reset();

	if(ns_name != nullptr) {
		done = !storage.createOrOpenNamespace(ns_name, false, nsIndex);
	}
}

void Storage::ItemIterator::reset()
{
	entryIndex = 0;
	page = storage.mPageManager.begin();
	done = false;
}

bool Storage::ItemIterator::next()
{
	if(!bool(*this)) {
		return false;
	}

	auto isIterableItem = [this]() {
		return (nsIndex != 0 && datatype != ItemType::BLOB && datatype != ItemType::BLOB_IDX);
	};

	auto isMultipageBlob = [this]() {
		return datatype == ItemType::BLOB_DATA && chunkIndex != uint8_t(VerOffset::VER_0_OFFSET) &&
			   chunkIndex != uint8_t(VerOffset::VER_1_OFFSET);
	};

	for(; page != storage.mPageManager.end(); ++page) {
		esp_err_t err;
		do {
			err = page->findItem(nsIndex, itemType, nullptr, entryIndex, *this);
			entryIndex += span;
			if(err == ESP_OK && isIterableItem() && !isMultipageBlob()) {
				return true;
			}
		} while(err != ESP_ERR_NVS_NOT_FOUND);

		entryIndex = 0;
	}

	done = true;
	return false;
}

String Storage::ItemIterator::ns_name() const
{
	auto it = std::find(storage.mNamespaces.begin(), storage.mNamespaces.end(), nsIndex);
	return it ? it->mName : nullptr;
}

HandlePtr Storage::open_handle(const char* ns_name, OpenMode open_mode)
{
	if(ns_name == nullptr) {
		mLastError = ESP_ERR_INVALID_ARG;
		return nullptr;
	}

	uint8_t nsIndex;
	if(!createOrOpenNamespace(ns_name, open_mode == OpenMode::ReadWrite, nsIndex)) {
		return nullptr;
	}

	auto handle = new(std::nothrow) Handle(*this, nsIndex, open_mode == OpenMode::ReadOnly);

	if(handle == nullptr) {
		mLastError = ESP_ERR_NO_MEM;
	} else {
		++mHandleCount;
		mLastError = ESP_OK;
	}

	return HandlePtr(handle);
}

} // namespace nvs
