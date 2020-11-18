/*****************************************************************************
 *   Copyright (c) 2020, Hobu, Inc. (info@hobu.co)                           *
 *                                                                           *
 *   All rights reserved.                                                    *
 *                                                                           *
 *   This program is free software; you can redistribute it and/or modify    *
 *   it under the terms of the GNU General Public License as published by    *
 *   the Free Software Foundation; either version 3 of the License, or       *
 *   (at your option) any later version.                                     *
 *                                                                           *
 ****************************************************************************/


#include <vector>

#include <pdal/util/FileUtils.hpp>

#include "Writer.hpp"
#include "Epf.hpp"
#include "../untwine/Common.hpp"
#include "../untwine/VoxelKey.hpp"

using namespace pdal;

namespace untwine
{
namespace epf
{

Writer::Writer(const std::string& directory, int numThreads, size_t pointSize) :
    m_directory(directory), m_pointSize(pointSize), m_pool(numThreads), m_stop(false)
{
    if (FileUtils::fileExists(directory))
    {
        if (!FileUtils::isDirectory(directory))
            throw Error("Specified output directory '" + directory + "' is not a directory.");
    }
    else
        FileUtils::createDirectory(directory);

    std::function<void()> f = std::bind(&Writer::run, this);
    while (numThreads--)
        m_pool.add(f);
}

std::string Writer::path(const VoxelKey& key)
{
    return m_directory + "/" + key.toString() + ".bin";
}


void Writer::enqueue(const VoxelKey& key, DataVecPtr data, size_t dataSize)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totals[key] += (dataSize / m_pointSize);
        m_queue.push_back({key, std::move(data), dataSize});
    }
    m_available.notify_one();
}

void Writer::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_available.notify_all();
    m_pool.join();
}

void Writer::run()
{
    while (true)
    {
        WriteData wd;

        // Loop waiting for data.
        while (true)
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            // Look for a queue entry that represents a key that we aren't already
            // actively processing.
            //ABELL - Perhaps a writer should grab and write *all* the entries in the queue
            //  that match the key we found.
            auto li = m_queue.begin();
            for (; li != m_queue.end(); ++li)
                if (std::find(m_active.begin(), m_active.end(), li->key) == m_active.end())
                    break;

            // If there is no data to process, exit if we're stopping. Wait otherwise.
            // If there is data to process, stick the key on the active list and
            // remove the item from the queue and break to do the actual write.
            if (li == m_queue.end())
            {
                if (m_stop)
                    return;
                m_available.wait(lock);
            }
            else
            {
                m_active.push_back(li->key);
                wd = std::move(*li);
                m_queue.erase(li);
                break;
            }
        }

        // Open the file. Write the data. Stick the buffer back on the cache.
        // Remove the key from the active key list.
        std::ofstream out(path(wd.key), std::ios::app | std::ios::binary);
        out.write(reinterpret_cast<const char *>(wd.data->data()), wd.dataSize);
        out.close();
        if (!out)
            throw Error("Failure writing to '" + path(wd.key) + "'.");
        m_bufferCache.replace(std::move(wd.data));

        std::lock_guard<std::mutex> lock(m_mutex);
        m_active.remove(wd.key);
    }
}

} // namespace epf
} // namespace untwine
