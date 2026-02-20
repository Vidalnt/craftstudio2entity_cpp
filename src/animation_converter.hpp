#ifndef CRAFTSTUDIO2ENTITY_CPP_ANIMATION_CONVERTER_HPP
#define CRAFTSTUDIO2ENTITY_CPP_ANIMATION_CONVERTER_HPP

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <variant>
#include "nlohmann/json.hpp"
#include "math_util.hpp"

using nlohmann::json;

namespace cs {

struct Keyframe {
    int frame;                    // Frame number (0-based)
    Vec3<double> value;
};

enum class ChannelType {
    POSITION,
    OFFSET_FROM_PIVOT,
    ROTATION,
    SIZE,
    STRETCH
};

struct NodeChannel {
    ChannelType type;
    std::vector<Keyframe> keyframes;
};

struct NodeAnimation {
    std::string nodeName;
    std::map<ChannelType, NodeChannel> channels;
};

struct Animation {
    std::string title;
    int duration;                 // Total frames (30 FPS)
    bool holdLastKeyframe;
    std::vector<NodeAnimation> nodes;
};

}

// ==================== BEDROCK ANIMATION (Output) ====================

namespace bedrock {

struct Keyframe {
    double time;                  // Time in seconds
    Vec3<double> value;
    // Bedrock supports linear interpolation by default, but also bezier, step, etc.
    std::optional<std::string> lerp_mode;
};

struct BoneChannel {
    std::optional<std::vector<Keyframe>> position;
    std::optional<std::vector<Keyframe>> rotation;
    std::optional<std::vector<Keyframe>> scale;
};

struct Animation {
    std::string identifier;       // Format: "animation.model.name"
    double animation_length;
    bool loop;
    std::map<std::string, BoneChannel> bones;
};

struct File {
    std::string format_version = "1.8.0";
    std::map<std::string, Animation> animations;
};

}
// ==================== CONVERTER ====================

class AnimationConverter {
public:
    static constexpr double CS_FPS = 60.0;

    /**
     * Converts a CraftStudio animation to Bedrock format
     *
     * @param csAnim Parsed CraftStudio animation
     * @param modelName Model name for generating the identifier
     * @return Bedrock structure ready for serialization
     */
    static bedrock::Animation convert(const cs::Animation& csAnim, const std::string& modelName);

    /**
     * Serializes to Bedrock JSON format
     */
    static json toJson(const bedrock::File& file);

    /**
     * Parses CraftStudio JSON
     */
    static cs::Animation parseCsJson(const json& j);

private:
    static double framesToSeconds(int frames);

    /**
     * Converts rotation from CraftStudio to Bedrock
     *
     * CraftStudio: YXZ order, degrees, custom coordinate system
     * Bedrock: XYZ order (roll-pitch-yaw), degrees
     *
     * Based on craftstudio_rot_to_entity_rot from the original code
     */
    static Vec3<double> convertRotation(const Vec3<double>& csRot);

    /**
     * Converts position from CraftStudio to Bedrock
     *
     * Note: In CraftStudio, animated position is RELATIVE to the rotation point
     * and added to it. In Bedrock, position is absolute relative to the bone.
     */
    static Vec3<double> convertPosition(const Vec3<double>& csPos);

    static Vec3<double> convertScale(const Vec3<double>& csStretch);

    static std::optional<std::reference_wrapper<std::optional<std::vector<bedrock::Keyframe>>>>
        mapChannel(bedrock::BoneChannel& bone, cs::ChannelType type);
};

#endif // CRAFTSTUDIO2ENTITY_CPP_ANIMATION_CONVERTER_HPP
