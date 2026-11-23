/******************************************************************************
    Ported from OBS Studio 33.0.1 (utility/ThumbnailManager.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix.
******************************************************************************/

/******************************************************************************
    Copyright (C) 2025 by Taylor Giampaolo <warchamp7@obsproject.com>
                          Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "LHThumbnailManager.hpp"

#include "LHThumbnailItem.hpp"
#include <QWidget>
#include "LHThumbnailView.hpp"


#include <QImageWriter>

constexpr int kMinimumThumbnailUpdateInterval = 100;
constexpr int kThumbnailUpdateInterval = 5000;

namespace {
bool updateItem(LHThumbnailItem *item)
{
	if (!item) {
		return false;
	}

	return item->update();
}
} // namespace

void LHThumbnailManager::ThumbnailCache::put(const std::string &key, const QPixmap &value)
{
	auto it = cacheMap.find(key);
	if (it != cacheMap.end()) {
		cacheList.erase(it->second);
		cacheMap.erase(it);
	}
	cacheList.emplace_front(key, value);
	cacheMap[key] = cacheList.begin();
	if (cacheMap.size() > maxSize) {
		const CacheEntry &lastEntry = cacheList.back();
		cacheMap.erase(lastEntry.first);
		cacheList.pop_back();
	}
}

std::optional<QPixmap> LHThumbnailManager::ThumbnailCache::get(const std::string &key)
{
	auto it = cacheMap.find(key);
	if (it == cacheMap.end()) {
		return std::nullopt;
	}

	auto entry = it->second;
	return entry->second;
}

LHThumbnailManager::LHThumbnailManager(QObject *parent) : QObject(parent)
{
	elapsedTimer.start();
	connect(&updateTimer, &QTimer::timeout, this, &LHThumbnailManager::updateTick);
	updateTickInterval(kMinimumThumbnailUpdateInterval);

	signalHandlers.emplace_back(obs_get_signal_handler(), "source_destroy", &LHThumbnailManager::obsSourceRemoved,
				    this);
}

LHThumbnailView *LHThumbnailManager::createView(QWidget *parent, obs_source_t *source)
{
	if (!source) {
		return new LHThumbnailView(parent, nullptr);
	}

	const char *uuidPointer = obs_source_get_uuid(source);
	if (!uuidPointer) {
		return new LHThumbnailView(parent, nullptr);
	}

	std::string uuid{uuidPointer};

	LHThumbnailItem *item = getThumbnailItem(uuid);
	LHThumbnailView *view = item->createView(parent);

	connect(view, &LHThumbnailView::updateRequested, this, [this](const std::string &uuid) {
		bool updateImmediately = true;
		addToPriorityQueue(uuid, updateImmediately);
	});

	return view;
}

std::optional<QPixmap> LHThumbnailManager::getCachedPixmap(const std::string &uuid)
{
	return thumbnailCache.get(uuid);
}

void LHThumbnailManager::createThumbnailItem(const std::string &uuid)
{
	QPointer<LHThumbnailItem> item = new LHThumbnailItem(uuid, this);
	thumbnailList[uuid] = item;

	if (item->isDefaultPixmap()) {
		updateQueue.emplace_front(uuid);
		updateTickInterval(kMinimumThumbnailUpdateInterval);
	} else {
		updateQueue.emplace_back(uuid);
	}

	connect(item, &LHThumbnailItem::noViewsRemaining, this, [this, uuid]() { deleteItemById(uuid); });
}

LHThumbnailItem *LHThumbnailManager::getThumbnailItem(const std::string &uuid)
{
	if (thumbnailList.find(uuid) == thumbnailList.end()) {
		createThumbnailItem(uuid);
	}

	return thumbnailList[uuid];
}

void LHThumbnailManager::obsSourceRemoved(void *data, calldata_t *params)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	const char *uuidPointer = obs_source_get_uuid(source);

	if (!uuidPointer) {
		return;
	}

	auto *manager = static_cast<LHThumbnailManager *>(data);
	QMetaObject::invokeMethod(manager, "deleteItemById", Qt::QueuedConnection, Q_ARG(std::string, uuidPointer));
}

void LHThumbnailManager::updateTickInterval(int newInterval)
{
	if (updateTimer.interval() != newInterval) {
		elapsedTimer.restart();
		updateTimer.start(newInterval);
	}
}

void LHThumbnailManager::updateNextItem(size_t cycleDepth)
{
	if (thumbnailList.empty()) {
		return;
	}

	QPointer<LHThumbnailItem> item;
	bool quickUpdate = false;

	if (!priorityQueue.empty()) {
		std::string uuid = priorityQueue.front();
		priorityQueue.pop_front();

		item = thumbnailList[uuid];
		updateQueue.emplace_back(std::move(uuid));

		if (!updateItem(item) && cycleDepth < thumbnailList.size()) {
			updateNextItem(cycleDepth + 1);
			return;
		}
	} else if (!updateQueue.empty()) {
		std::string uuid = updateQueue.front();

		updateQueue.pop_front();
		item = thumbnailList[uuid];
		updateQueue.emplace_back(std::move(uuid));

		if (item->isDefaultPixmap()) {
			quickUpdate = true;
		}

		if (!updateItem(item) && cycleDepth < thumbnailList.size()) {
			updateNextItem(cycleDepth + 1);
			return;
		}
	}

	int nextIntervalMS = kThumbnailUpdateInterval;
	if (!priorityQueue.empty() || quickUpdate) {
		nextIntervalMS = kMinimumThumbnailUpdateInterval;
	}

	updateTickInterval(nextIntervalMS);
}

void LHThumbnailManager::updateTick()
{
	updateNextItem();
}

void LHThumbnailManager::removeIdFromQueues(const std::string &uuid)
{
	auto it = std::find(updateQueue.begin(), updateQueue.end(), uuid);
	if (it != updateQueue.end()) {
		updateQueue.erase(it);
	}

	it = std::find(priorityQueue.begin(), priorityQueue.end(), uuid);
	if (it != priorityQueue.end()) {
		priorityQueue.erase(it);
	}
}

void LHThumbnailManager::addToPriorityQueue(const std::string &uuid, bool immediate)
{
	// Skip if uuid is already at the front of the priority queue
	if (!priorityQueue.empty() && priorityQueue[0] == uuid) {
		return;
	}

	removeIdFromQueues(uuid);

	if (immediate) {
		priorityQueue.emplace_front(uuid);

		qint64 elapsed = elapsedTimer.elapsed();
		if (elapsed > kMinimumThumbnailUpdateInterval) {
			updateTick();
		}
	} else {
		priorityQueue.emplace_back(uuid);
	}
}

void LHThumbnailManager::addItemToCache(std::string &uuid, QPixmap &pixmap)
{
	if (pixmap.isNull()) {
		return;
	}

	thumbnailCache.put(uuid, pixmap);
}

void LHThumbnailManager::deleteItem(LHThumbnailItem *item)
{
	deleteItemById(item->getUuid());
	item->deleteLater();
}

void LHThumbnailManager::deleteItemById(const std::string &uuid)
{
	auto entry = thumbnailList.find(uuid);
	if (entry != thumbnailList.end()) {
		removeIdFromQueues(uuid);

		QPointer<LHThumbnailItem> item = entry->second;

		if (item && !item->isDefaultPixmap()) {
			std::string uuid = item->getUuid();
			QPixmap pixmap = item->getPixmap();
			addItemToCache(uuid, pixmap);
		}

		thumbnailList.erase(uuid);
	}
}

static LHThumbnailManager *s_thumbnails = nullptr;

LHThumbnailManager *lhThumbnails()
{
	if (!s_thumbnails)
		s_thumbnails = new LHThumbnailManager(nullptr);
	return s_thumbnails;
}

void lhThumbnailsShutdown()
{
	delete s_thumbnails;
	s_thumbnails = nullptr;
}
