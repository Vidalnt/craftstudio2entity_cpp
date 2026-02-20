#include <iostream>
#include <ctime>
#include <fstream>
#include <set>
#include <chrono>

#include "math_util.hpp"
#include "bedrock_model.hpp"
#include "craftstudio_model.hpp"
#include "model_io.hpp"
#include "animation_converter.hpp"

static constexpr Vec2i VISIBLE_BOUNDS{1, 2};
static const Vec3<double> VISIBLE_BOUNDS_OFFSET{0, 0, 0};
static constexpr Vec2i TEXTURE_SIZE{128, 128};

/**
 * Converts a CraftStudioBlock cube to a BedrockEntityCube.
 *
 * @param block     the block
 * @param pivot_cs  the block's pivot in CraftStudio world space (NOT Bedrock, z not yet negated)
 * @param isRoot    true if this block has no parent
 */
BedrockEntityCube blockToCube(const CraftStudioBlock &block,
                              const Vec3<double> &pivot_cs,
                              bool isRoot) {
    const Vec3<double> &size   = block.size;
    const Vec3<double> &offset = block.offsetFromPivot;

    double ox, oy, oz;

    if (isRoot) {
        // For root bones the offset is already baked into the pivot,
        // so the cube is simply centred on the pivot.
        ox =  pivot_cs.x - size.x / 2.0;
        oy =  pivot_cs.y - size.y / 2.0;
        oz = -pivot_cs.z - size.z / 2.0;   // flip Z for Bedrock
    } else {
        // For child bones the pivot is the rotation point (joint).
        // The cube centre is at pivot + offsetFromPivot.
        double cx = pivot_cs.x + offset.x;
        double cy = pivot_cs.y + offset.y;
        double cz = pivot_cs.z + offset.z;   // still in CS space

        ox =  cx - size.x / 2.0;
        oy =  cy - size.y / 2.0;
        oz = -cz - size.z / 2.0;             // flip Z for Bedrock
    }

    return BedrockEntityCube{Vec3<double>{ox, oy, oz}, size, block.texOffset};
}

/**
 * Recursively converts a CraftStudioBlock (and its children) to BedrockEntityBones.
 *
 * Coordinate convention
 * ---------------------
 * CraftStudio uses a left-handed Y-up system where Z points INTO the screen.
 * Bedrock uses a right-handed Y-up system where Z points OUT of the screen.
 * Conversion: Bedrock.z = -CraftStudio.z
 *
 * Pivot rules (derived from the original CraftStudio renderer)
 * ------------------------------------------------------------
 * Root bone : pivot_cs = block.position + block.offsetFromPivot
 *             (the offsetFromPivot of a root bone shifts the ENTIRE model up/down)
 * Child bone: pivot_cs = parentPivot_cs + block.position
 *             (position is relative to the parent's pivot, not its cube centre)
 *
 * The pivot that is stored in Bedrock JSON is pivot_cs with Z negated.
 *
 * @param geometry        target geometry
 * @param block           block to convert
 * @param parentName      name of the parent bone (nullptr for root)
 * @param parentPivot_cs  parent's pivot in CraftStudio world space
 * @param isRoot          true when this block has no parent
 */
void blockToBone(BedrockEntityGeometry &geometry,
                 const CraftStudioBlock &block,
                 const std::string      *parentName,
                 Vec3<double>            parentPivot_cs,
                 bool                    isRoot) {

    // --- Compute this bone's pivot in CS world space ---
    Vec3<double> pivot_cs;
    if (isRoot) {
        // The root's own offsetFromPivot acts as a global Y-lift for the whole model.
        pivot_cs = block.position + block.offsetFromPivot;
    } else {
        // Children are positioned relative to their parent's pivot (joint).
        pivot_cs = parentPivot_cs + block.position;
    }

    // --- Convert pivot to Bedrock space (flip Z) ---
    auto *const pivot_bedrock = new Vec3<double>{pivot_cs.x, pivot_cs.y, -pivot_cs.z};

    // --- Rotation ---
    auto *const rotation = new Vec3<double>{};
    *rotation = craftstudio_rot_to_entity_rot(block.rotation);

    // --- Scale (stretch) ---
    Vec3<double> *scale = nullptr;
    if (block.stretch.x != 1.0 || block.stretch.y != 1.0 || block.stretch.z != 1.0) {
        scale = new Vec3<double>{block.stretch};
        if (block.stretch.x != 1.0 || block.stretch.y != 1.0 || block.stretch.z != 1.0)
            std::cerr << "WARNING: stretch on bone \"" << block.name
                      << "\" is not fully supported by Bedrock entity geometry." << std::endl;
    }

    // --- Build bone and add cube ---
    BedrockEntityBone bone{block.name, parentName, pivot_bedrock, rotation, scale};
    bone.push_cube(blockToCube(block, pivot_cs, isRoot));
    geometry.push_bone(std::move(bone));

    // --- Recurse into children ---
    // Children always use pivot_cs (CS space, Z not negated) as their parent pivot.
    for (const CraftStudioBlock &child : block.get_children()) {
        blockToBone(geometry, child, &block.name, pivot_cs, false);
    }
}


BedrockEntityModel convert(const CraftStudioModel &csModel) {
    BedrockEntityModel result{};
    auto *geometry = new BedrockEntityGeometry{
            VISIBLE_BOUNDS,
            VISIBLE_BOUNDS_OFFSET,
            TEXTURE_SIZE
    };

    for (const CraftStudioBlock &block : csModel.get_blocks())
        blockToBone(*geometry, block, nullptr, {0, 0, 0}, true);

    result.insert(csModel.title, geometry);
    return result;
}

int exit_with_err(const std::string &error) {
    std::cerr << error << std::endl;
    exit(1);
#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCDFAInspection"
    return 1;
#pragma clang diagnostic pop
}

bool file_exists(const char *path) {
    if (FILE *file = fopen(path, "r")) {
        fclose(file);
        return true;
    } else return false;
}

long current_time_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int main(int argc, const char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input> <output> [flags]" << std::endl;
        std::cerr << "  input:  .csjsmodel (model) or .csjsanim (animation)" << std::endl;
        std::cerr << "  output: .json (entity geometry or animation)" << std::endl;
        std::cerr << "  flags:  r=replace, v=verbose" << std::endl;
        return 1;
    }

    std::string inPath = argv[1];
    std::string outPath = argv[2];

    // Detect type by extension or content
    bool isAnimation = inPath.find(".csjsanim") != std::string::npos;

    if (!isAnimation) {
        // Verify content if extension is not clear
        std::ifstream test(inPath);
        json testJson;
        if (test >> testJson && testJson.contains("nodeAnimations")) {
            isAnimation = true;
        }
        test.close();
    }

    std::set<char> flags{};
    if (argc > 3)
        for (char c : std::string{argv[3]})
            flags.insert(c);

    if (file_exists(outPath.c_str()) && !flags.count('r'))
        exit_with_err(outPath + " already exists!");

    if (isAnimation) {
        std::ifstream inFile(inPath);
        if (!inFile.good()) {
            return exit_with_err("Cannot open input file: " + inPath);
        }

        json csJson;
        inFile >> csJson;
        inFile.close();

        try {
            auto csAnim = AnimationConverter::parseCsJson(csJson);
            std::cerr << "Animation loaded: " << csAnim.title
                      << " (" << csAnim.duration << " frames)" << std::endl;

            // Extract model name from output or use default
            std::string modelName = "model";
            size_t lastSlash = outPath.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                modelName = outPath.substr(lastSlash + 1);
                size_t dot = modelName.find('.');
                if (dot != std::string::npos) {
                    modelName = modelName.substr(0, dot);
                }
            }

            auto bedrockAnim = AnimationConverter::convert(csAnim, modelName);
            bedrock::File file;
            file.animations[bedrockAnim.identifier] = bedrockAnim;

            std::ofstream outFile(outPath);
            if (!outFile.good()) {
                return exit_with_err("Cannot open output file: " + outPath);
            }

            auto outJson = AnimationConverter::toJson(file);
            outFile << std::setw(4) << outJson << std::endl;
            outFile.close();

            std::cerr << "Animation converted successfully!" << std::endl;
            std::cerr << "  Length: " << bedrockAnim.animation_length << "s" << std::endl;
            std::cerr << "  Loop: " << (bedrockAnim.loop ? "yes" : "no") << std::endl;
            std::cerr << "  Bones: " << bedrockAnim.bones.size() << std::endl;

        } catch (const std::exception& e) {
            return exit_with_err(std::string("Animation conversion failed: ") + e.what());
        }

    } else {
        if (!std::ifstream(inPath).good())
            exit_with_err(inPath + " must be a file!");

        long time = current_time_millis();
        {
             std::ifstream csFile(inPath);
             CraftStudioModel csModel = read_model(csFile);
             csFile.close();
             std::cerr << "CraftStudio model loaded." << std::endl;

             BedrockEntityModel entityModel = convert(csModel);
             std::cerr << "Model converted to Bedrock entity." << std::endl;

             std::ofstream entityFile(outPath);
             write_model(entityModel, entityFile);
             entityFile.close();
             std::cerr << "Bedrock entity model written." << std::endl;
        }
        time = current_time_millis() - time;

        std::cerr << "Done! (" << time << " ms)" << std::endl;
    }

    return 0;
}
