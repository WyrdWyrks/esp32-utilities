#pragma once

#include "FilesystemUtils.h"

namespace FilesystemModule
{
// Manager class for the LittleFS filesystem
    class Manager
    {
    private:
        static constexpr const char *TAG = "FilesystemManager";

    public:
        Manager() {}
        ~Manager() {}

        void InitializeFilesystem()
        {
            ESP_LOGI(TAG, "FilesystemManager::Init");
            Utilities::Init();
        }
    };
}