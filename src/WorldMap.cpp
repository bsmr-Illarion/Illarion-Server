//  illarionserver - server for the game Illarion
//  Copyright 2011 Illarion e.V.
//
//  This file is part of illarionserver.
//
//  illarionserver is free software: you can redistribute it and/or modify
//  it under the terms of the GNU Affero General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  illarionserver is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Affero General Public License for more details.
//
//  You should have received a copy of the GNU Affero General Public License
//  along with illarionserver.  If not, see <http://www.gnu.org/licenses/>.


#include "WorldMap.hpp"
#include "Map.hpp"
#include "Logger.hpp"

#include <algorithm>
#include <stdexcept>
#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <chrono>

void WorldMap::clear() {
    world_map.clear();
    maps.clear();
}

bool WorldMap::intersects(const Map &map) const {
    for(const auto &testMap : maps) {
        if (map.intersects(testMap)) {
            Logger::error(LogFacility::Script)
                << "Could not insert map " << map.getName()
                << " because it intersects with " << testMap.getName()
                << Log::end;

            return true;
        }
    }
    
    return false;
}

Field &WorldMap::at(const position &pos) {
    try {
        return maps.at(world_map.at(pos)).at(pos.x, pos.y);
    } catch (std::out_of_range &e) {
        throw FieldNotFound();
    }
}

const Field &WorldMap::at(const position &pos) const {
    try {
        return maps.at(world_map.at(pos)).at(pos.x, pos.y);
    } catch (std::out_of_range &e) {
        throw FieldNotFound();
    }
}

Field &WorldMap::walkableNear(position &pos) {
    try {
        return maps.at(world_map.at(pos)).walkableNear(pos.x, pos.y);
    } catch (std::out_of_range &e) {
        throw FieldNotFound();
    }
}

const Field &WorldMap::walkableNear(position &pos) const {
    try {
        return maps.at(world_map.at(pos)).walkableNear(pos.x, pos.y);
    } catch (std::out_of_range &e) {
        throw FieldNotFound();
    }
}

bool WorldMap::insert(Map&& newMap) {
    if (intersects(newMap)) return false;

    maps.push_back(std::move(newMap));

    auto &map = maps.back();
    const auto z = map.getLevel();

    for (auto x = map.getMinX(); x <= map.getMaxX(); ++x) {
        for (auto y = map.getMinY(); y <= map.getMaxY(); ++y) {
            position p(x, y, z);
            world_map[p] = maps.size() - 1;
        }
    }

    return true;
}

bool WorldMap::allMapsAged() {
    using std::chrono::steady_clock;
    using std::chrono::milliseconds;

    auto startTime = steady_clock::now();

    while (ageIndex < maps.size() &&
           steady_clock::now() - startTime < milliseconds(10)) {
        maps[ageIndex++].age();
    }

    if (ageIndex < maps.size()) {
        return false;
    }

    ageIndex = 0;
    return true;
}

bool WorldMap::import(const std::string &importDir,
                      const std::string &mapName) {
    bool success = true;

    try {
        auto map = createMapFromHeaderFile(importDir, mapName);
        success = map.import(importDir, mapName) && insert(std::move(map));
    } catch (MapError &) {
        success = false;
    }

    if (!success) {
        Logger::alert(LogFacility::Script) << "---> Could not import " << mapName
                                          << Log::end;
    }

    return success;
}

Map WorldMap::createMapFromHeaderFile(const std::string &importDir,
                                      const std::string &mapName) {
    const std::string fileName = mapName + ".tiles.txt";
    std::ifstream headerFile(importDir + fileName);

    if (!headerFile) {
        Logger::error(LogFacility::Script) << "Could not open file: " << fileName
                                          << Log::end;
        throw MapError();
    }

    int lineNumber = 0;

    auto version = readHeaderLine(mapName, 'V', headerFile, lineNumber);

    if (version != 2) {
        Logger::error(LogFacility::Script)
            << fileName << ": incorrect version in line " << lineNumber
            << "! Expected: 2" << Log::end;
        throw MapError();
    }

    position origin;
    origin.z = readHeaderLine(mapName, 'L', headerFile, lineNumber);
    origin.x = readHeaderLine(mapName, 'X', headerFile, lineNumber);
    origin.y = readHeaderLine(mapName, 'Y', headerFile, lineNumber);
    
    auto width = readHeaderLine(mapName, 'W', headerFile, lineNumber);

    if (width <= 0) {
        Logger::error(LogFacility::Script)
            << fileName << ": width has to be positive in line " << lineNumber
            << Log::end;
        throw MapError();
    }

    if (origin.x > std::numeric_limits<int16_t>::max() - width + 1) {
        Logger::error(LogFacility::Script)
            << fileName << ": x + width - 1 must not exceed "
            << std::numeric_limits<int16_t>::max() << " in line " << lineNumber
            << Log::end;
        throw MapError();
    }

    auto height = readHeaderLine(mapName, 'H', headerFile, lineNumber);

    if (height <= 0) {
        Logger::error(LogFacility::Script)
            << fileName << ": height has to be positive in line " << lineNumber
            << Log::end;
        throw MapError();
    }

    if (origin.y > std::numeric_limits<int16_t>::max() - height + 1) {
        Logger::error(LogFacility::Script)
            << fileName << ": y + height - 1 must not exceed "
            << std::numeric_limits<int16_t>::max() << " in line " << lineNumber
            << Log::end;
        throw MapError();
    }

    return Map(mapName, origin, width, height);
}

const boost::regex headerExpression {R"(^(.): (-?\d+)$)"};

int16_t WorldMap::readHeaderLine(const std::string &mapName, char header,
                                 std::ifstream &headerFile, int &lineNumber) {
    using boost::regex_match;
    using boost::smatch;
    std::string line;

    while (std::getline(headerFile, line)) {
        ++lineNumber;

        if (!isCommentOrEmpty(line)) {
            smatch matches;

            if (regex_match(line, matches, headerExpression) &&
                matches[1] == header) {
                try {
                    return boost::lexical_cast<int16_t>(matches[2]);
                } catch (boost::bad_lexical_cast &) {
                    break;
                }
            }
        }
    }

    Logger::error(LogFacility::Script) << mapName << ": expected header '"
                                      << header << ": <int16_t>' but found '"
                                      << line << "' in line " << lineNumber
                                      << Log::end;
    throw MapError();
}

bool WorldMap::isCommentOrEmpty(const std::string &line) {
    return line.length() == 0 || line[0] == '#';
}

bool WorldMap::exportTo(const std::string &exportDir) const {
    for (const auto &map : maps) {
        int16_t minX = map.getMinX();
        int16_t minY = map.getMinY();
        // create base filename
        std::string filebase = exportDir + "e_" + std::to_string(minX)
                               + "_" + std::to_string(minY)
                               + "_" + std::to_string(map.getLevel()) + ".";
        // export fields file
        std::ofstream fieldsf(filebase + "tiles.txt");
        // export items file
        std::ofstream itemsf(filebase + "items.txt");
        // export warps file
        std::ofstream warpsf(filebase + "warps.txt");

        if (!fieldsf.good() || !itemsf.good() || !warpsf.good()) {
            Logger::error(LogFacility::World) << "Could not open output files for item export: " << filebase << "*.txt" << Log::end;
            return false;
        }

        // export tiles header
        fieldsf << "V: 2" << std::endl;
        fieldsf << "L: " << map.getLevel() << std::endl;
        fieldsf << "X: " << minX << std::endl;
        fieldsf << "Y: " << minY << std::endl;
        fieldsf << "W: " << map.getWidth() << std::endl;
        fieldsf << "H: " << map.getHeight() << std::endl;

        // iterate over the map and export...
        short int x, y;

        for (y = minY; y <= map.getMaxY(); ++y) {
            for (x = minX; x <= map.getMaxX(); ++x) {
                const Field &field = map.at(x, y);

                fieldsf << x-minX << ";" << y-minY << ";" << field.getTileCode() << ";" << field.getMusicId() << std::endl;

                if (field.isWarp()) {
                    position target;
                    field.getWarp(target);
                    warpsf << x-minX << ";" << y-minY << ";" << target.x << ";" << target.y << ";" << target.z << std::endl;
                }

                for (const auto &item : field.getExportItems()) {
                    itemsf << x-minX << ";" << y-minY << ";" << item.getId() << ";" << item.getQuality();

                    std::for_each(item.getDataBegin(), item.getDataEnd(), [&](const std::pair<std::string, std::string> &data) {
                        using boost::algorithm::replace_all;

                        std::string key = data.first;
                        std::string value = data.second;

                        replace_all(key, "\\", "\\\\");
                        replace_all(key, "=", "\\=");
                        replace_all(key, ";", "\\;");
                        replace_all(value, "\\", "\\\\");
                        replace_all(value, "=", "\\=");
                        replace_all(value, ";", "\\;");

                        itemsf << ";" << key << "=" << value;
                    });

                    itemsf << std::endl;
                }
            }
        }

        fieldsf.close();
        itemsf.close();
        warpsf.close();

    }

    return true;
}

bool WorldMap::loadFromDisk(const std::string &path) {
    std::ifstream mapinitfile(path + "_initmaps",
                              std::ios::binary | std::ios::in);

    if (!mapinitfile) {
        Logger::error(LogFacility::World)
            << "Error while loading maps: could not open "
            << (path + "_initmaps") << Log::end;
        return false;
    } else {
        unsigned short int size;
        mapinitfile.read((char *) & size, sizeof(size));
        Logger::info(LogFacility::World) << "Loading " << size << " maps."
                                         << Log::end;

        int16_t tZ_Level;
        int16_t tMin_X;
        int16_t tMin_Y;

        uint16_t tWidth;
        uint16_t tHeight;

        char mname[ 200 ];

        for (int i = 0; i < size; ++i) {
            mapinitfile.read((char *) & tZ_Level, sizeof(tZ_Level));
            mapinitfile.read((char *) & tMin_X, sizeof(tMin_X));
            mapinitfile.read((char *) & tMin_Y, sizeof(tMin_Y));

            mapinitfile.read((char *) & tWidth, sizeof(tWidth));
            mapinitfile.read((char *) & tHeight, sizeof(tHeight));

            auto map = Map{"previously saved map",
                           position{tMin_X, tMin_Y, tZ_Level}, tWidth, tHeight};

            sprintf(mname, "%s_%6d_%6d_%6d", path.c_str(), tZ_Level, tMin_X,
                    tMin_Y);

            if (map.Load(mname)) {
                insert(std::move(map));
            }
        }

        return true;
    }
}

void WorldMap::saveToDisk(const std::string &prefix) const {
    std::ofstream mapinitfile(prefix + "_initmaps", std::ios::binary | std::ios::out | std::ios::trunc);

    if (!mapinitfile.good()) {
        Logger::error(LogFacility::World) << "Could not create initmaps!" << Log::end;
    } else {
        uint16_t size = maps.size();
        Logger::info(LogFacility::World) << "Saving " << size << " maps." << Log::end;
        mapinitfile.write((char *) & size, sizeof(size));
        char mname[200];

        for (const auto &map : maps) {
            const auto level = map.getLevel();
            const auto x = map.getMinX();
            const auto y = map.getMinY();
            const auto width = map.getWidth();
            const auto height = map.getHeight();
            mapinitfile.write((char *) &level, sizeof(level));
            mapinitfile.write((char *) &x, sizeof(x));
            mapinitfile.write((char *) &y, sizeof(y));
            mapinitfile.write((char *) &width, sizeof(width));
            mapinitfile.write((char *) &height, sizeof(height));

            sprintf(mname, "%s_%6d_%6d_%6d", prefix.c_str(), level, x, y);
            map.Save(mname);
        }

        mapinitfile.close();
    }
}

bool WorldMap::createMap(const std::string &name, const position &origin,
                         uint16_t width, uint16_t height, uint16_t tile) {
    return insert({name, origin, width, height, tile});
}
