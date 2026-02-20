//
// Created by user on 4/14/19.
//

#ifndef CRAFTSTUDIO2ENTITY_CPP_MODEL_IO_HPP
#define CRAFTSTUDIO2ENTITY_CPP_MODEL_IO_HPP

#include <string>
#include <iostream>
#include <iomanip>
#include <ios>
#include <cmath>

#include "nlohmann/json.hpp"
#include "io_util.hpp"
#include "bedrock_model.hpp"
#include "craftstudio_model.hpp"


using nlohmann::json;

#define FORMAT_VERSION "1.12.0"

// IN

template<class T>
Vec3<T> vec3_from_json(const json &json) noexcept(false) {
    if (!json.is_array())
        throw std::ios_base::failure("vec2i must be read from an array");
    if (json.size() != 3)
        throw std::ios_base::failure("vec3 must be 3 elements long");
    return Vec3<T>{json[0], json[1], json[2]};
}

Vec2i vec2i_from_json(const json &json) noexcept(false) {
    if (!json.is_array())
        throw std::ios_base::failure("vec2i must be read from an array");
    if (json.size() != 2)
        throw std::ios_base::failure("vec2i must be 2 elements long");
    return Vec2i{json[0], json[1]};
}

CraftStudioBlock block_from_json(const json &root) {
    const std::string &name = root["name"];
    Vec3<double> position = vec3_from_json<double>(root["position"]);
    Vec3<double> offsetFromPivot = vec3_from_json<double>(root["offsetFromPivot"]);
    Vec3<double> size = vec3_from_json<double>(root["size"]);
    Vec3<double> rotation = vec3_from_json<double>(root["rotation"]);
    Vec2i texOffset = vec2i_from_json(root["texOffset"]);
    Vec3<double> stretch{1.0, 1.0, 1.0};

    if (root.contains("vertexCoords")) {
        const json &array = root["vertexCoords"];
        if (array.is_array()) {
            auto get_vertex = [&](int index) -> Vec3<double> {
                const json &v = array[index];
                return {v[0], v[1], v[2]};
            };
            
            Vec3<double> v3 = get_vertex(3);
            Vec3<double> v2 = get_vertex(2);
            Vec3<double> v0 = get_vertex(0);
            Vec3<double> v6 = get_vertex(6);

            double stretchX = size.x != 0.0 ? std::abs(v2.x - v3.x) / size.x : 1.0;
            double stretchY = size.y != 0.0 ? std::abs(v0.y - v3.y) / size.y : 1.0;
            double stretchZ = size.z != 0.0 ? std::abs(v6.z - v3.z) / size.z : 1.0;
            stretch = {stretchX, stretchY, stretchZ};
        }
    }

    json json_children = root["children"];
    CraftStudioBlock block{name, position, offsetFromPivot, size, stretch, rotation, texOffset};

    for (const json &json_child : json_children) {
        CraftStudioBlock child = block_from_json(json_child);
        block.push_child(child);
    }



    return block;
}

CraftStudioModel model_from_json(const json &root) {
    const std::string &title = root["title"];
    const json &tree = root["tree"];

    CraftStudioModel model{title};
    for (const json& json_block : tree) {
        CraftStudioBlock block = block_from_json(json_block);
        model.push_block(block);
    }

    return model;
}

CraftStudioModel read_model(std::istream &stream) noexcept(false) {
    json root;
    stream >> root;
    return model_from_json(root);
}

// OUT

template<class T>
inline json vec3_to_json(const Vec3<T> &v) {
    return json::array({v.x, v.y, v.z});
}

inline json vec2i_to_json(const Vec2i &v) {
    return json::array({v.x, v.y});
}

inline json cube_to_json(const BedrockEntityCube &cube) {
    return json::object({{"origin", vec3_to_json(cube.origin)},
                         {"size",   vec3_to_json(cube.size)},
                         {"uv",     vec2i_to_json(cube.uv)}});
}

json bone_to_json(const BedrockEntityBone &bone) {
    json root = json::object();
    root["name"] = bone.name;

    if (bone.has_parent())
        root["parent"] = *bone.parent;

    if (bone.has_pivot()) {
        root["pivot"] = vec3_to_json(*bone.pivot);
    }
    if (bone.has_rotation()) {
        if (!bone.has_pivot())
            std::cerr << ("WARNING: Bone \"" + bone.name + "\" has a rotation but no pivot");
        root["rotation"] = vec3_to_json(*bone.rotation);
    }

    if (bone.has_scale()) {
        root["scale"] = vec3_to_json(*bone.scale);
    }

    json jsonCubes = json::array();

    for (const BedrockEntityCube &cube : bone.get_cubes())
        jsonCubes.push_back(cube_to_json(cube));
    root["cubes"] = jsonCubes;

    return root;
}

json geometry_to_json(const std::string &identifier, const BedrockEntityGeometry &geometry) {
    json root = json::object();
    json description = json::object();

    description["identifier"] = identifier;

    const Vec2i &texture_size = geometry.texture_size;
    description["texture_width"] = texture_size.x;
    description["texture_height"] = texture_size.y;

    const Vec2i &visible_bounds = geometry.visible_bounds;
    description["visible_bounds_width"] = visible_bounds.x;
    description["visible_bounds_height"] = visible_bounds.y;
    description["visible_bounds_offset"] = vec3_to_json(geometry.visible_bounds_offset);

    root["description"] = description;

    json json_bones = json::array();
    for (const BedrockEntityBone &bone : geometry.get_bones())
        json_bones.push_back(bone_to_json(bone));
    root["bones"] = json_bones;

    return root;
}

json model_to_json(const BedrockEntityModel &model) {
    json root = json::object();
    root["format_version"] = FORMAT_VERSION;

    json geometries = json::array();
    for (const auto &entry : model.get_geometries()) {
        std::string identifier = entry.first;
        if (identifier.find("geometry.") == std::string::npos) {
             identifier = "geometry." + identifier;
        }
        geometries.push_back(geometry_to_json(identifier, *entry.second));
    }
    root["minecraft:geometry"] = geometries;

    return root;
}

void write_model(const BedrockEntityModel &model, std::ostream &stream) {
    json json = model_to_json(model);

    stream << std::setw(4) << json;
}

#endif //CRAFTSTUDIO2ENTITY_CPP_MODEL_IO_HPP
