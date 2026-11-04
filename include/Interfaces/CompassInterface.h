#pragma once

#include <ArduinoJson.h>

class CompassInterface
{
public:
    virtual int GetAzimuth() = 0;
    virtual void PrintRawValues() = 0;

    // Debug readout: "magMoniker"/"accelMoniker" strings and "mag"/"accel"
    // [x, y, z] arrays in each chip's own axes (mag hard-iron corrected).
    virtual void GetRawValues(JsonDocument &doc) {}

    // Calibration
    virtual void BeginCalibration() = 0;
    virtual void IterateCalibration() = 0;
    virtual void EndCalibration() = 0;
    virtual void GetCalibrationData(JsonDocument &doc) = 0;
    virtual void SetCalibrationData(JsonDocument &doc) = 0;
};