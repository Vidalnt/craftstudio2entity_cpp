#include "animation_converter.hpp"
#include <cmath>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

cs::Animation AnimationConverter::parseCsJson(const json& j) {
    cs::Animation anim;

    anim.title = j.value("title", "untitled");
    anim.duration = j.value("duration", 30);
    anim.holdLastKeyframe = j.value("holdLastKeyframe", false);

    if (!j.contains("nodeAnimations")) {
        throw std::runtime_error("Missing nodeAnimations field");
    }

    const auto& nodeAnims = j["nodeAnimations"];
    for (auto& [nodeName, nodeData] : nodeAnims.items()) {
        cs::NodeAnimation node;
        node.nodeName = nodeName;

        const std::map<std::string, cs::ChannelType> fieldMap = {
            {"position", cs::ChannelType::POSITION},
            {"offsetFromPivot", cs::ChannelType::OFFSET_FROM_PIVOT},
            {"rotation", cs::ChannelType::ROTATION},
            {"size", cs::ChannelType::SIZE},
            {"stretch", cs::ChannelType::STRETCH}
        };

        for (auto& [fieldName, channelType] : fieldMap) {
            if (!nodeData.contains(fieldName)) continue;

            const auto& fieldObj = nodeData[fieldName];
            if (!fieldObj.is_object()) continue;

            cs::NodeChannel channel;
            channel.type = channelType;

            for (auto& [frameStr, valueArr] : fieldObj.items()) {
                int frame = std::stoi(frameStr);
                if (!valueArr.is_array() || valueArr.size() != 3) continue;

                Vec3<double> val{
                    valueArr[0].get<double>(),
                    valueArr[1].get<double>(),
                    valueArr[2].get<double>()
                };

                channel.keyframes.push_back(cs::Keyframe{frame, val});
            }

            std::sort(channel.keyframes.begin(), channel.keyframes.end(),
                [](const auto& a, const auto& b) { return a.frame < b.frame; });

            if (!channel.keyframes.empty()) {
                node.channels[channelType] = std::move(channel);
            }
        }

        anim.nodes.push_back(std::move(node));
    }

    return anim;
}

double AnimationConverter::framesToSeconds(int frames) {
    return static_cast<double>(frames) / CS_FPS;
}

Vec3<double> AnimationConverter::convertRotation(const Vec3<double>& csRot) {
    // Reuse existing model conversion: handles YXZ -> ZXY (Bedrock) with coordinate transform
    return craftstudio_rot_to_entity_rot(csRot);
}

Vec3<double> AnimationConverter::convertPosition(const Vec3<double>& csPos) {
    // CraftStudio negates Y/Z during JSON parse; apply Z flip for Bedrock coordinate system
    return Vec3<double>{csPos.x, csPos.y, -csPos.z};
}

Vec3<double> AnimationConverter::convertScale(const Vec3<double>& csStretch) {
    return csStretch;
}

bedrock::Animation AnimationConverter::convert(const cs::Animation& csAnim,
                                               const std::string& modelName) {
    bedrock::Animation result;

    result.identifier = "animation." + modelName + "." + csAnim.title;
    result.animation_length = framesToSeconds(csAnim.duration);
    result.loop = !csAnim.holdLastKeyframe;

    for (const auto& csNode : csAnim.nodes) {
        bedrock::BoneChannel bone;

        for (const auto& [type, channel] : csNode.channels) {
            std::vector<bedrock::Keyframe> keyframes;

            for (const auto& kf : channel.keyframes) {
                bedrock::Keyframe bk;
                bk.time = framesToSeconds(kf.frame);

                switch (type) {
                    case cs::ChannelType::ROTATION:
                        bk.value = convertRotation(kf.value);
                        if (!bone.rotation) bone.rotation = std::vector<bedrock::Keyframe>{};
                        bone.rotation->push_back(bk);
                        break;

                    case cs::ChannelType::POSITION:
                        bk.value = convertPosition(kf.value);
                        if (!bone.position) bone.position = std::vector<bedrock::Keyframe>{};
                        bone.position->push_back(bk);
                        break;

                    case cs::ChannelType::OFFSET_FROM_PIVOT:
                        // Bedrock has no direct offsetFromPivot; requires bone pivot knowledge to simulate.
                        // In CS this animates vertex offset, not bone position—complex to convert directly.
                        std::cerr << "WARNING: offsetFromPivot animation not fully supported" << std::endl;
                        break;

                    case cs::ChannelType::STRETCH:
                        bk.value = convertScale(kf.value);
                        if (!bone.scale) bone.scale = std::vector<bedrock::Keyframe>{};
                        bone.scale->push_back(bk);
                        break;

                    case cs::ChannelType::SIZE:
                        // Animated SIZE not supported in Bedrock format
                        std::cerr << "WARNING: size animation not supported in Bedrock" << std::endl;
                        break;
                }
            }
        }

        if (bone.position || bone.rotation || bone.scale) {
            result.bones[csNode.nodeName] = std::move(bone);
        }
    }

    return result;
}

static json keyframesToJson(const std::vector<bedrock::Keyframe>& kfs) {
    json obj = json::object();
    for (const auto& kf : kfs) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(4) << kf.time;
        std::string timeKey = ss.str();

        // Remove trailing zeros and unnecessary decimal point
        while (timeKey.find('.') != std::string::npos &&
               timeKey.length() > 1 &&
               (timeKey.back() == '0' || timeKey.back() == '.')) {
            if (timeKey.back() == '.') {
                timeKey.pop_back();
                break;
            }
            timeKey.pop_back();
        }
        if (timeKey.empty() || timeKey == "-") timeKey = "0";

        json valueArr = json::array({kf.value.x, kf.value.y, kf.value.z});

        // Round small values to zero for cleaner output
        for (auto& v : valueArr) {
            double val = v.get<double>();
            if (std::abs(val) < 0.0001) {
                v = 0.0;
            } else {
                v = std::round(val * 10000.0) / 10000.0;
            }
        }

        obj[timeKey] = valueArr;
    }
    return obj;
}

json AnimationConverter::toJson(const bedrock::File& file) {
    json root;
    root["format_version"] = file.format_version;

    json anims = json::object();
    for (const auto& [id, anim] : file.animations) {
        json animObj = json::object();
        animObj["animation_length"] = anim.animation_length;

        if (anim.loop) {
            animObj["loop"] = true;
        }

        json bonesObj = json::object();
        for (const auto& [boneName, bone] : anim.bones) {
            json boneObj = json::object();

            if (bone.rotation) {
                boneObj["rotation"] = keyframesToJson(*bone.rotation);
            }
            if (bone.position) {
                boneObj["position"] = keyframesToJson(*bone.position);
            }
            if (bone.scale) {
                boneObj["scale"] = keyframesToJson(*bone.scale);
            }

            if (!boneObj.empty()) {
                bonesObj[boneName] = boneObj;
            }
        }

        if (!bonesObj.empty()) {
            animObj["bones"] = bonesObj;
        }

        anims[id] = animObj;
    }

    root["animations"] = anims;
    return root;
}
