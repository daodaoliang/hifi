//
//  Created by Bradley Austin Davis on 2015/09/05
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Procedural.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <gpu/Batch.h>
#include <SharedUtil.h>
#include <NumericalConstants.h>
#include <GLMHelpers.h>

#include "ProceduralCommon_frag.h"

#include "Logging.h"

Q_LOGGING_CATEGORY(procedural, "hifi.gpu.procedural")


// Userdata parsing constants
static const QString PROCEDURAL_USER_DATA_KEY = "ProceduralEntity";
static const QString URL_KEY = "shaderUrl";
static const QString VERSION_KEY = "version";
static const QString UNIFORMS_KEY = "uniforms";
static const QString CHANNELS_KEY = "channels";

// Shader replace strings
static const std::string PROCEDURAL_BLOCK = "//PROCEDURAL_BLOCK";
static const std::string PROCEDURAL_COMMON_BLOCK = "//PROCEDURAL_COMMON_BLOCK";
static const std::string PROCEDURAL_VERSION = "//PROCEDURAL_VERSION";

static const std::string STANDARD_UNIFORM_NAMES[Procedural::NUM_STANDARD_UNIFORMS] = {
    "iDate",
    "iGlobalTime",
    "iFrameCount",
    "iWorldScale",
    "iWorldPosition",
    "iWorldOrientation",
    "iChannelResolution"
};

bool operator ==(const ProceduralData& a, const ProceduralData& b) {
    return (
        (a.version == b.version) &&
        (a.shaderUrl == b.shaderUrl) &&
        (a.uniforms == b.uniforms) &&
        (a.channels == b.channels));
}


QJsonValue ProceduralData::getProceduralData(const QString& proceduralJson) {
    if (proceduralJson.isEmpty()) {
        return QJsonValue();
    }

    QJsonParseError parseError;
    auto doc = QJsonDocument::fromJson(proceduralJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return QJsonValue();
    }

    return doc.object()[PROCEDURAL_USER_DATA_KEY];
}

ProceduralData ProceduralData::parse(const QString& userDataJson) {
    ProceduralData result;
    result.parse(getProceduralData(userDataJson).toObject());
    return result;
}

void ProceduralData::parse(const QJsonObject& proceduralData) {
    if (proceduralData.isEmpty()) {
        return;
    }

    {
        auto versionJson = proceduralData[VERSION_KEY];
        if (versionJson.isDouble()) {
            version = (uint8_t)(floor(versionJson.toDouble()));
            // invalid version
            if (!(version == 1 || version == 2)) {
                return;
            }
        } else {
            // All unversioned shaders default to V1
            version = 1;
        }
    }

    auto rawShaderUrl = proceduralData[URL_KEY].toString();
    shaderUrl = DependencyManager::get<ResourceManager>()->normalizeURL(rawShaderUrl);
    
    // Empty shader URL isn't valid
    if (shaderUrl.isEmpty()) {
        return;
    }

    uniforms = proceduralData[UNIFORMS_KEY].toObject();
    channels = proceduralData[CHANNELS_KEY].toArray();
}

// Example
//{
//    "ProceduralEntity": {
//        "shaderUrl": "file:///C:/Users/bdavis/Git/hifi/examples/shaders/test.fs",
//    }
//}

Procedural::Procedural() {
    _transparentState->setCullMode(gpu::State::CULL_NONE);
    _transparentState->setDepthTest(true, true, gpu::LESS_EQUAL);
    _transparentState->setBlendFunction(true,
        gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::INV_SRC_ALPHA,
        gpu::State::FACTOR_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::ONE);

}

void Procedural::setProceduralData(const ProceduralData& proceduralData) {
    if (proceduralData == _data) {
        return;
    }

    _dirty = true;
    _enabled = false;

    if (proceduralData.uniforms != _data.uniforms) {
        _data.uniforms = proceduralData.uniforms;
        _uniformsDirty = true;
    }

    if (proceduralData.channels != _data.channels) {
        _data.channels = proceduralData.channels;
        // Must happen on the main thread
        auto textureCache = DependencyManager::get<TextureCache>();
        size_t channelCount = std::min(MAX_PROCEDURAL_TEXTURE_CHANNELS, (size_t)proceduralData.channels.size());
        size_t channel;
        for (channel = 0; channel < MAX_PROCEDURAL_TEXTURE_CHANNELS; ++channel) {
            if (channel < channelCount) {
                QString url = proceduralData.channels.at((int)channel).toString();
                _channels[channel] = textureCache->getTexture(QUrl(url));
            } else {
                // Release those textures no longer in use
                _channels[channel] = textureCache->getTexture(QUrl());
            }
        }
        _channelsDirty = true;
    }

    if (proceduralData.shaderUrl != _data.shaderUrl) {
        _data.shaderUrl = proceduralData.shaderUrl;
        _shaderDirty = true;
        const auto& shaderUrl = _data.shaderUrl;
        _networkShader.reset();
        _shaderPath.clear();

        if (shaderUrl.isEmpty()) {
            return;
        }

        if (!shaderUrl.isValid()) {
            qCWarning(procedural) << "Invalid shader URL: " << shaderUrl;
            return;
        }

        if (shaderUrl.isLocalFile()) {
            if (!QFileInfo(shaderUrl.toLocalFile()).exists()) {
                qCWarning(procedural) << "Invalid shader URL, missing local file: " << shaderUrl;
                return;
            }
            _shaderPath = shaderUrl.toLocalFile();
        } else {
            _networkShader = ShaderCache::instance().getShader(shaderUrl);
        }
    }

    _enabled = true;
}

bool Procedural::isReady() const {
    if (!_enabled) {
        return false;
    }

    if (!_hasStartedFade) {
        _fadeStartTime = usecTimestampNow();
    }

    // Do we have a network or local shader, and if so, is it loaded?
    if (_shaderPath.isEmpty() && (!_networkShader || !_networkShader->isLoaded())) {
        return false;
    }

    // Do we have textures, and if so, are they loaded?
    for (size_t i = 0; i < MAX_PROCEDURAL_TEXTURE_CHANNELS; ++i) {
        if (_channels[i] && !_channels[i]->isLoaded()) {
            return false;
        }
    }

    if (!_hasStartedFade) {
        _hasStartedFade = true;
        _isFading = true;
    }

    return true;
}

void Procedural::prepare(gpu::Batch& batch, const glm::vec3& position, const glm::vec3& size, const glm::quat& orientation) {
    _entityDimensions = size;
    _entityPosition = position;
    _entityOrientation = glm::mat3_cast(orientation);
    if (!_shaderPath.isEmpty()) {
        auto lastModified = (quint64)QFileInfo(_shaderPath).lastModified().toMSecsSinceEpoch();
        if (lastModified > _shaderModified) {
            QFile file(_shaderPath);
            file.open(QIODevice::ReadOnly);
            _shaderSource = QTextStream(&file).readAll();
            _shaderDirty = true;
            _shaderModified = lastModified;
        }
    } else if (_networkShader && _networkShader->isLoaded()) {
        _shaderSource = _networkShader->_source;
    }

    if (!_opaquePipeline || !_transparentPipeline || _shaderDirty) {
        if (!_vertexShader) {
            _vertexShader = gpu::Shader::createVertex(_vertexSource);
        }

        // Build the fragment shader
        std::string fragmentShaderSource = _fragmentSource;
        size_t replaceIndex = fragmentShaderSource.find(PROCEDURAL_COMMON_BLOCK);
        if (replaceIndex != std::string::npos) {
            fragmentShaderSource.replace(replaceIndex, PROCEDURAL_COMMON_BLOCK.size(), ProceduralCommon_frag);
        }

        replaceIndex = fragmentShaderSource.find(PROCEDURAL_VERSION);
        if (replaceIndex != std::string::npos) {
            if (_data.version == 1) {
                fragmentShaderSource.replace(replaceIndex, PROCEDURAL_VERSION.size(), "#define PROCEDURAL_V1 1");
            } else if (_data.version == 2) {
                fragmentShaderSource.replace(replaceIndex, PROCEDURAL_VERSION.size(), "#define PROCEDURAL_V2 1");
            }
        }
        replaceIndex = fragmentShaderSource.find(PROCEDURAL_BLOCK);
        if (replaceIndex != std::string::npos) {
            fragmentShaderSource.replace(replaceIndex, PROCEDURAL_BLOCK.size(), _shaderSource.toLocal8Bit().data());
        }

        // Leave this here for debugging
        // qCDebug(procedural) << "FragmentShader:\n" << fragmentShaderSource.c_str();

        _fragmentShader = gpu::Shader::createPixel(fragmentShaderSource);
        _shader = gpu::Shader::createProgram(_vertexShader, _fragmentShader);

        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding(std::string("iChannel0"), 0));
        slotBindings.insert(gpu::Shader::Binding(std::string("iChannel1"), 1));
        slotBindings.insert(gpu::Shader::Binding(std::string("iChannel2"), 2));
        slotBindings.insert(gpu::Shader::Binding(std::string("iChannel3"), 3));
        gpu::Shader::makeProgram(*_shader, slotBindings);

        _opaquePipeline = gpu::Pipeline::create(_shader, _opaqueState);
        _transparentPipeline = gpu::Pipeline::create(_shader, _transparentState);
        for (size_t i = 0; i < NUM_STANDARD_UNIFORMS; ++i) {
            const std::string& name = STANDARD_UNIFORM_NAMES[i];
            _standardUniformSlots[i] = _shader->getUniforms().findLocation(name);
        }
        _start = usecTimestampNow();
        _frameCount = 0;
    }

    batch.setPipeline(isFading() ? _transparentPipeline : _opaquePipeline);

    if (_shaderDirty || _uniformsDirty) {
        setupUniforms();
    }

    if (_shaderDirty || _uniformsDirty || _channelsDirty) {
        setupChannels(_shaderDirty || _uniformsDirty);
    }

    _shaderDirty = _uniformsDirty = _channelsDirty = false;

    for (auto lambda : _uniforms) {
        lambda(batch);
    }

    static gpu::Sampler sampler;
    static std::once_flag once;
    std::call_once(once, [&] {
        gpu::Sampler::Desc desc;
        desc._filter = gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR;
    });

    for (size_t i = 0; i < MAX_PROCEDURAL_TEXTURE_CHANNELS; ++i) {
        if (_channels[i] && _channels[i]->isLoaded()) {
            auto gpuTexture = _channels[i]->getGPUTexture();
            if (gpuTexture) {
                gpuTexture->setSampler(sampler);
                gpuTexture->setAutoGenerateMips(true);
            }
            batch.setResourceTexture((gpu::uint32)i, gpuTexture);
        }
    }
}

void Procedural::setupUniforms() {
    _uniforms.clear();
    // Set any userdata specified uniforms 
    foreach(QString key, _data.uniforms.keys()) {
        std::string uniformName = key.toLocal8Bit().data();
        int32_t slot = _shader->getUniforms().findLocation(uniformName);
        if (gpu::Shader::INVALID_LOCATION == slot) {
            continue;
        }
        QJsonValue value = _data.uniforms[key];
        if (value.isDouble()) {
            float v = value.toDouble();
            _uniforms.push_back([=](gpu::Batch& batch) {
                batch._glUniform1f(slot, v);
            });
        } else if (value.isArray()) {
            auto valueArray = value.toArray();
            switch (valueArray.size()) {
                case 0:
                    break;

                case 1: {
                    float v = valueArray[0].toDouble();
                    _uniforms.push_back([=](gpu::Batch& batch) {
                        batch._glUniform1f(slot, v);
                    });
                    break;
                }

                case 2: {
                    glm::vec2 v{ valueArray[0].toDouble(), valueArray[1].toDouble() };
                    _uniforms.push_back([=](gpu::Batch& batch) {
                        batch._glUniform2f(slot, v.x, v.y);
                    });
                    break;
                }

                case 3: {
                    glm::vec3 v{
                        valueArray[0].toDouble(),
                        valueArray[1].toDouble(),
                        valueArray[2].toDouble(),
                    };
                    _uniforms.push_back([=](gpu::Batch& batch) {
                        batch._glUniform3f(slot, v.x, v.y, v.z);
                    });
                    break;
                }

                default:
                case 4: {
                    glm::vec4 v{
                        valueArray[0].toDouble(),
                        valueArray[1].toDouble(),
                        valueArray[2].toDouble(),
                        valueArray[3].toDouble(),
                    };
                    _uniforms.push_back([=](gpu::Batch& batch) {
                        batch._glUniform4f(slot, v.x, v.y, v.z, v.w);
                    });
                    break;
                }
            }
        }
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[TIME]) {
        _uniforms.push_back([=](gpu::Batch& batch) {
            // Minimize floating point error by doing an integer division to milliseconds, before the floating point division to seconds
            float time = (float)((usecTimestampNow() - _start) / USECS_PER_MSEC) / MSECS_PER_SECOND;
            batch._glUniform(_standardUniformSlots[TIME], time);
        });
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[DATE]) {
        _uniforms.push_back([=](gpu::Batch& batch) {
            QDateTime now = QDateTime::currentDateTimeUtc();
            QDate date = now.date();
            QTime time = now.time();
            vec4 v;
            v.x = date.year();
            // Shadertoy month is 0 based
            v.y = date.month() - 1;
            // But not the day... go figure
            v.z = date.day();
            float fractSeconds = (time.msec() / 1000.0f);
            v.w = (time.hour() * 3600) + (time.minute() * 60) + time.second() + fractSeconds;
            batch._glUniform(_standardUniformSlots[DATE], v);
        });
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[FRAME_COUNT]) {
        _uniforms.push_back([=](gpu::Batch& batch) {
            batch._glUniform(_standardUniformSlots[FRAME_COUNT], ++_frameCount);
        });
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[SCALE]) {
        // FIXME move into the 'set once' section, since this doesn't change over time
        _uniforms.push_back([=](gpu::Batch& batch) {
            batch._glUniform(_standardUniformSlots[SCALE], _entityDimensions);
        });
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[ORIENTATION]) {
        // FIXME move into the 'set once' section, since this doesn't change over time
        _uniforms.push_back([=](gpu::Batch& batch) {
            batch._glUniform(_standardUniformSlots[ORIENTATION], _entityOrientation);
        });
    }

    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[POSITION]) {
        // FIXME move into the 'set once' section, since this doesn't change over time
        _uniforms.push_back([=](gpu::Batch& batch) {
            batch._glUniform(_standardUniformSlots[POSITION], _entityPosition);
        });
    }
}

void Procedural::setupChannels(bool shouldCreate) {
    if (gpu::Shader::INVALID_LOCATION != _standardUniformSlots[CHANNEL_RESOLUTION]) {
        if (!shouldCreate) {
            // Instead of modifying the last element, just remove and recreate it.
            _uniforms.pop_back();
        }
        _uniforms.push_back([=](gpu::Batch& batch) {
            vec3 channelSizes[MAX_PROCEDURAL_TEXTURE_CHANNELS];
            for (size_t i = 0; i < MAX_PROCEDURAL_TEXTURE_CHANNELS; ++i) {
                if (_channels[i]) {
                    channelSizes[i] = vec3(_channels[i]->getWidth(), _channels[i]->getHeight(), 1.0);
                }
            }
            batch._glUniform3fv(_standardUniformSlots[CHANNEL_RESOLUTION], MAX_PROCEDURAL_TEXTURE_CHANNELS, &channelSizes[0].x);
        });
    }
}

glm::vec4 Procedural::getColor(const glm::vec4& entityColor) {
    if (_data.version == 1) {
        return glm::vec4(1);
    }
    return entityColor;
}
