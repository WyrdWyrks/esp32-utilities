#pragma once

#include <ArduinoJson.h>
#include <Preferences.h>
#include <Arduino.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <string>

namespace FilesystemModule
{
    class SettingsInterface; // forward declaration required by SettingsMap below

    using SettingsMap = std::vector<std::shared_ptr<FilesystemModule::SettingsInterface>>;
    
    // Fallback for clamp if C++17 is not available
    template<typename T>
    T clamp(T value, T min_val, T max_val) {
        return (value < min_val) ? min_val : (value > max_val) ? max_val : value;
    }

    class SettingsInterface {
    public:
        std::string key;
        static constexpr const char* preference_namespace = "Settings";
        static constexpr const char *value_key = "cfgVal";
        static constexpr const char *type_key = "cfgType";
        static constexpr const char *write_key = "return";
        
        SettingsInterface(std::string key) : key(std::move(key)) {}
        
        virtual void resetToDefault() = 0;

        // Serializes setting to json object for UI consumption (includes metadata like min/max/step for numeric settings, or labels for enum settings)
        virtual void toJson(JsonObject& obj) const = 0;

        // Adds setting value under key name to JsonDocument
        virtual void toJsonSettingsDoc(JsonDocument& doc) = 0;

        // Deserializes setting from json object (e.g. from UI input) TODO: Maybe change key to "return" as UI currently uses
        virtual void fromJson(JsonObjectConst& obj) = 0;

        // Saves setting value to Preferences. If prefs is not already open this
        // opens the namespace read-write and closes it; if the caller has
        // already opened it (e.g. to batch-write many settings in a single
        // transaction) the existing handle is reused and left open for the
        // caller to close.
        virtual void saveToPreferences(Preferences& prefs) const = 0;

        // Loads setting value from Preferences (if exists). Same lifecycle rule
        // as saveToPreferences, but opens the namespace read-only when it owns
        // the handle.
        virtual void loadFromPreferences(Preferences& prefs) = 0;

        virtual std::string getType() const = 0;

        virtual ~SettingsInterface() = default;

    protected:
        const char * _TAG = "SettingsInterface";
    };

    template<typename T>
    class Setting : public SettingsInterface {
    public:
        T value;
        const T default_value;

        Setting(std::string key, T default_val)
            : SettingsInterface(std::move(key)), value(default_val), default_value(default_val) {}

        // 
        void toJsonSettingsDoc(JsonDocument& doc) override {
            doc[key] = value;
        }

        void resetToDefault() override { value = default_value; }
    };

    class IntSetting : public Setting<int> {
    public:
        const int min, max, step;

        static constexpr const char * min_key = "minVal";
        static constexpr const char * max_key = "maxVal";
        static constexpr const char * step_key = "incVal";
        static constexpr const char * type = "int";

        IntSetting(std::string key, int def, int min, int max, int step)
        
            : Setting(std::move(key), def), min(min), max(max), step(step) {}

        void toJson(JsonObject& obj) const override {
            obj[value_key] = value;
            obj[min_key] = min;
            obj[max_key] = max;
            obj[step_key] = step;
            obj[type_key] = type;
        }

        void fromJson(JsonObjectConst& obj) override {
            JsonVariantConst v = obj[write_key];
            value = clamp(v.isNull() ? default_value : v.as<int>(), min, max);
        }

        void saveToPreferences(Preferences& prefs) const override {
            bool opened = prefs.begin(preference_namespace, false);
            prefs.putInt(key.c_str(), value);
            if (opened) prefs.end();
        }

        void loadFromPreferences(Preferences& prefs) override {
            bool opened = prefs.begin(preference_namespace, true);
            value = clamp((int)prefs.getInt(key.c_str(), default_value), min, max);
            if (opened) prefs.end();
        }

        std::string getType() const override {
            return type;
        }
    };

    class FloatSetting : public Setting<float> {
    public:
        const float min, max, step;

        static constexpr const char* min_key = "minVal";
        static constexpr const char* max_key = "maxVal";
        static constexpr const char* step_key = "incVal";
        static constexpr const char* type = "float";

        FloatSetting(std::string key, float def, float min, float max, float step)
            : Setting(std::move(key), def), min(min), max(max), step(step) {}

        void toJson(JsonObject& obj) const override {
            obj[value_key] = value;
            obj[min_key] = min;
            obj[max_key] = max;
            obj[step_key] = step;
            obj[type_key] = type;
        }

        void fromJson(JsonObjectConst& obj) override {
            JsonVariantConst v = obj[write_key];
            value = clamp(v.isNull() ? default_value : v.as<float>(), min, max);
        }

        void saveToPreferences(Preferences& prefs) const override {
            bool opened = prefs.begin(preference_namespace, false);
            prefs.putFloat(key.c_str(), value);
            if (opened) prefs.end();
        }

        void loadFromPreferences(Preferences& prefs) override {
            bool opened = prefs.begin(preference_namespace, true);
            value = clamp(prefs.getFloat(key.c_str(), default_value), min, max);
            if (opened) prefs.end();
        }

        std::string getType() const override {
            return type;
        }
    };

    class EnumSetting : public Setting<int> {
    public:
        const std::vector<std::string> labels;
        const std::vector<int> values;

        static constexpr const char* labels_key = "valTxt";
        static constexpr const char* values_key = "vals";
        static constexpr const char* type = "enum";

        EnumSetting(std::string key, int def,
                    std::vector<std::string> labels,
                    std::vector<int> values)
            : Setting(std::move(key), def),
            labels(std::move(labels)),
            values(std::move(values)) {}

        void toJson(JsonObject& obj) const override {
            obj[value_key] = currentIndex();
            obj[type_key] = type;
            ArduinoJson::JsonArray arr_labels = obj[labels_key].to<ArduinoJson::JsonArray>();
            ArduinoJson::JsonArray arr_values = obj[values_key].to<ArduinoJson::JsonArray>();
            for (const auto& l : labels) arr_labels.add(l);
            for (const auto& v : values) arr_values.add(v);
        }

        void fromJson(JsonObjectConst& obj) override {
            JsonVariantConst v = obj[write_key];
            int idx = v.isNull() ? defaultIndex() : v.as<int>();
            setByIndex(idx);
        }

        void saveToPreferences(Preferences& prefs) const override {
            bool opened = prefs.begin(preference_namespace, false);
            prefs.putInt(key.c_str(), currentIndex());
            if (opened) prefs.end();
        }

        void loadFromPreferences(Preferences& prefs) override {
            bool opened = prefs.begin(preference_namespace, true);
            int idx = prefs.getInt(key.c_str(), defaultIndex());
            if (opened) prefs.end();
            setByIndex(idx);
        }

        std::string getType() const override {
            return type;
        }

    private:
        int currentIndex() const {
            return std::max(0, indexOfValue(value));
        }

        int defaultIndex() const {
            return std::max(0, indexOfValue(default_value));
        }

        int indexOfValue(int v) const {
            auto it = std::find(values.begin(), values.end(), v);
            if (it == values.end()) return -1;
            return std::distance(values.begin(), it);
        }

        void setByIndex(int idx) {
            if (idx >= 0 && idx < (int)values.size()) {
                value = values[idx];
            } else {
                value = default_value;
            }
        }
    };

    class StringSetting : public Setting<std::string> {
    public:
        const size_t max_length;
        const std::string allowed_chars;

        static constexpr const char* max_length_key = "maxLen";
        static constexpr const char* allowed_chars_key = "charList";
        static constexpr const char* type = "string";

        StringSetting(std::string key, std::string def,
                    size_t max_length,
                    std::string allowed_chars = 
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz"
                    "!@#$%^&*()-_=+[]{}|;:',.<>?/`~\"\\"
                    "0123456789 "
                    )
            : Setting(std::move(key), std::move(def)),
            max_length(max_length),
            allowed_chars(std::move(allowed_chars)) {}

        void toJson(JsonObject& obj) const override {
            obj[value_key] = value;
            obj[type_key] = type;
            obj[max_length_key] = max_length;
            if (!allowed_chars.empty()) {
                obj[allowed_chars_key] = allowed_chars;
            }
        }

        void fromJson(JsonObjectConst& obj) override {
            JsonVariantConst v = obj[write_key];
            std::string incoming = v.isNull() ? default_value : v.as<std::string>();
            value = sanitize(incoming);
        }

        void saveToPreferences(Preferences& prefs) const override {
            bool opened = prefs.begin(preference_namespace, false);
            prefs.putString(key.c_str(), value.c_str());
            if (opened) prefs.end();
        }

        void loadFromPreferences(Preferences& prefs) override {
            bool opened = prefs.begin(preference_namespace, true);
            if (prefs.isKey(key.c_str())) {
                std::string loaded = prefs.getString(key.c_str(), default_value.c_str()).c_str();
                value = sanitize(loaded);
            } else {
                value = default_value;
            }
            if (opened) prefs.end();
        }

        std::string getType() const override {
            return type;
        }

    private:
        std::string sanitize(const std::string& input) const {
            std::string result;
            result.reserve(std::min(input.size(), max_length));
            for (char c : input) {
                if (result.size() >= max_length) break;
                if (allowed_chars.empty() || allowed_chars.find(c) != std::string::npos) {
                    result += c;
                }
            }
            return result;
        }
    };

    class BoolSetting : public Setting<bool> {
    public:
        static constexpr const char* type = "bool";

        BoolSetting(std::string key, bool def)
            : Setting(std::move(key), def) {}

        void toJson(JsonObject& obj) const override {
            obj[value_key] = value;
            obj[type_key] = type;
        }

        void fromJson(JsonObjectConst& obj) override {
            JsonVariantConst v = obj[write_key];
            if (v.isNull())
            {
                ESP_LOGW(_TAG, "Setting is null. Assigning default value");
                value = default_value;
            }
            else
            {
                value = v.as<bool>();
                ESP_LOGI(_TAG, "Assigning value %d to bool setting.", value);
            }
        }

        void saveToPreferences(Preferences& prefs) const override {
            bool opened = prefs.begin(preference_namespace, false);
            prefs.putBool(key.c_str(), value);
            if (opened) prefs.end();
        }

        void loadFromPreferences(Preferences& prefs) override {
            bool opened = prefs.begin(preference_namespace, true);
            value = prefs.getBool(key.c_str(), default_value);
            if (opened) prefs.end();
        }

        std::string getType() const override {
            return type;
        }
    };
}