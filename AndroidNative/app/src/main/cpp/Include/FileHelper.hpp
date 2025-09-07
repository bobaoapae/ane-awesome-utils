//
// Created by reveny on 29/08/2023.
//

#include <link.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/system_properties.h>

namespace FileHelper {
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

    inline bool findStringInMaps(const std::string& targetLib, const std::string& targetString) {
        std::ifstream mapsFile("/proc/self/maps");
        if (!mapsFile.is_open()) {
            perror("fopen maps");
            return false;
        }

        int memFd = open("/proc/self/mem", O_RDONLY);
        if (memFd == -1) {
            perror("open mem");
            return false;
        }

        std::string line;
        uintptr_t start, end;

        while (std::getline(mapsFile, line)) {
            std::istringstream iss(line);
            std::string addressRange, permissions, offset, dev, inode, pathname;

            if (iss >> addressRange >> permissions >> offset >> dev >> inode) {
                if (iss >> pathname && pathname.find(targetLib) != std::string::npos) {
                    if (sscanf(addressRange.c_str(), "%zx-%zx", &start, &end) != 2) {
                        continue;
                    }
                    size_t size = end - start;

                    if (permissions.find('r') != std::string::npos && size >= targetString.size()) {
                        size_t len = targetString.size();
                        char buffer[len];
                        for (size_t off = 0; off <= size - len; ++off) {
                            off_t seekPos = static_cast<off_t>(start + off);
                            if (lseek(memFd, seekPos, SEEK_SET) == -1) {
                                continue;
                            }
                            ssize_t bytesRead = read(memFd, buffer, len);
                            if (bytesRead == static_cast<ssize_t>(len) && memcmp(buffer, targetString.c_str(), len) == 0) {
                                close(memFd);
                                return true;
                            }
                        }
                    }
                }
            }
        }

        close(memFd);
        return false;
    }

    inline std::vector<std::string> readFile(const std::string& path) {
        std::vector<std::string> returnVal = std::vector<std::string>();
        char line[512] = {0};
        unsigned int line_count = 0;

        FILE *file = fopen(path.c_str(), "r");
        if (!file) {
            return returnVal;
        }

        while (fgets(line, 512, file)) {
            line_count++;
            returnVal.emplace_back(line);
        }

        fclose(file);
        return returnVal;
    }

    inline std::vector<std::string> listFilesInDirectory(const std::string& path) {
        std::vector<std::string> files;
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(path.c_str())) != nullptr) {
            while ((ent = readdir(dir)) != nullptr) {
                files.emplace_back(ent->d_name);
            }
            closedir(dir);
        }

        return files;
    }

    inline bool fileExists(const std::string& path) {
        FILE *file;
        if ((file = fopen(path.c_str(), "r"))) {
            fclose(file);
            return true;
        } else {
            return false;
        }
    }

    inline std::vector<dl_phdr_info> getLoadedLibraries() {
        std::vector<dl_phdr_info> infos{};
        dl_iterate_phdr([](struct dl_phdr_info *info, size_t size, void *data) -> int {
            (void) size;

            if ((info)->dlpi_name == nullptr) {
                return 0;
            }

            ((std::vector<dl_phdr_info> *) data)->push_back(*info);
            return 0;
        }, &infos);

        return infos;
    }

    inline std::string getSystemProperty(const std::string& input) {
        char prop_out[PROP_VALUE_MAX];
        if (__system_property_get(input.c_str(), prop_out)) {
            return {prop_out};
        }

        return "";
    }
}
