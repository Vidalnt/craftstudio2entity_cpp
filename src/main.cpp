#include <iostream>
#include <ctime>
#include <fstream>
#include <set>
#include <chrono>
#include <filesystem>
#include <functional>

#include "math_util.hpp"
#include "bedrock_model.hpp"
#include "craftstudio_model.hpp"
#include "model_io.hpp"
#include "animation_converter.hpp"

namespace fs = std::filesystem;

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

// ─── Single-file helpers ────────────────────────────────────────────────────

/**
 * Detects if a file is a CraftStudio animation (.csjsmodelanim) by reading
 * its JSON content and checking for the "nodeAnimations" key.
 * Returns false for .csjsmodel (geometry) files.
 */
bool detect_is_animation(const std::string &path) {
    std::ifstream test(path);
    if (!test.good()) return false;
    json testJson;
    if (test >> testJson && testJson.contains("nodeAnimations"))
        return true;
    return false;
}

/**
 * Converts a single .csjsmodel file and writes the Bedrock geometry JSON.
 * Returns true on success.
 */
bool convert_model_file(const std::string &inPath,
                        const std::string &outPath,
                        const std::set<char> &flags) {
    if (file_exists(outPath.c_str()) && !flags.count('r')) {
        std::cerr << "SKIP (exists): " << outPath << std::endl;
        return false;
    }
    std::ifstream csFile(inPath);
    if (!csFile.good()) {
        std::cerr << "ERROR: Cannot open " << inPath << std::endl;
        return false;
    }
    try {
        CraftStudioModel csModel = read_model(csFile);
        csFile.close();
        BedrockEntityModel entityModel = convert(csModel);
        // Ensure output directory exists
        fs::create_directories(fs::path(outPath).parent_path());
        std::ofstream entityFile(outPath);
        if (!entityFile.good()) {
            std::cerr << "ERROR: Cannot write " << outPath << std::endl;
            return false;
        }
        write_model(entityModel, entityFile);
        entityFile.close();
        std::cerr << "  [model] " << inPath << "\n         -> " << outPath << std::endl;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "ERROR converting model " << inPath << ": " << e.what() << std::endl;
        return false;
    }
}

/**
 * Converts a single .csjsanim file and writes the Bedrock animation JSON.
 * Returns true on success.
 */
bool convert_anim_file(const std::string &inPath,
                       const std::string &outPath,
                       const std::set<char> &flags) {
    if (file_exists(outPath.c_str()) && !flags.count('r')) {
        std::cerr << "SKIP (exists): " << outPath << std::endl;
        return false;
    }
    std::ifstream inFile(inPath);
    if (!inFile.good()) {
        std::cerr << "ERROR: Cannot open " << inPath << std::endl;
        return false;
    }
    try {
        json csJson;
        inFile >> csJson;
        inFile.close();

        auto csAnim = AnimationConverter::parseCsJson(csJson);

        // Derive model name from stem of output filename
        std::string modelName = fs::path(outPath).stem().string();
        if (modelName.empty()) modelName = "model";

        auto bedrockAnim = AnimationConverter::convert(csAnim, modelName);
        bedrock::File file;
        file.animations[bedrockAnim.identifier] = bedrockAnim;

        // Ensure output directory exists
        fs::create_directories(fs::path(outPath).parent_path());
        std::ofstream outFile(outPath);
        if (!outFile.good()) {
            std::cerr << "ERROR: Cannot write " << outPath << std::endl;
            return false;
        }
        auto outJson = AnimationConverter::toJson(file);
        outFile << std::setw(4) << outJson << std::endl;
        outFile.close();
        std::cerr << "  [anim]  " << inPath << "\n         -> " << outPath << std::endl;
        return true;
    } catch (const std::exception &e) {
        std::cerr << "ERROR converting animation " << inPath << ": " << e.what() << std::endl;
        return false;
    }
}

// ─── main ───────────────────────────────────────────────────────────────────

int main(int argc, const char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input> <output> [flags]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Single-file mode:" << std::endl;
        std::cerr << "  input:  path to a .csjsmodel (geometry) or .csjsmodelanim (animation) file" << std::endl;
        std::cerr << "  output: path to the destination .json file" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Folder (batch) mode:" << std::endl;
        std::cerr << "  input:  path to a folder containing .csjsmodel / .csjsmodelanim files" << std::endl;
        std::cerr << "  output: destination pattern, e.g.  D:\\out\\anims\\*.json" << std::endl;
        std::cerr << "          The '*' is replaced by each source file's stem." << std::endl;
        std::cerr << std::endl;
        std::cerr << "  flags:  r=replace existing files, v=verbose" << std::endl;
        return 1;
    }

    std::string inPath  = argv[1];
    std::string outPath = argv[2];

    std::set<char> flags{};
    if (argc > 3)
        for (char c : std::string{argv[3]})
            flags.insert(c);

    // ── FOLDER / BATCH MODE ──────────────────────────────────────────────────
    if (fs::is_directory(inPath)) {
        // Parse the output pattern: must contain '*'
        // e.g. D:\Programs\example\anims\*.json
        size_t starPos = outPath.find('*');
        if (starPos == std::string::npos) {
            return exit_with_err(
                "When input is a folder the output must be a pattern like: "
                "D:\\path\\to\\out\\*.json");
        }

        std::string outDir    = outPath.substr(0, starPos);        // "D:\path\to\out\"
        std::string outExt    = outPath.substr(starPos + 1);       // ".json"

        // Remove trailing slash from outDir (we'll join with stem below)
        while (!outDir.empty() && (outDir.back() == '/' || outDir.back() == '\\'))
            outDir.pop_back();

        if (!fs::exists(outDir)) {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            if (ec) return exit_with_err("Cannot create output directory: " + outDir);
        }

        int converted = 0, errors = 0;
        long time = current_time_millis();

        for (const auto &entry : fs::recursive_directory_iterator(inPath)) {
            if (!entry.is_regular_file()) continue;

            const fs::path srcPath  = entry.path();
            const std::string ext   = srcPath.extension().string();
            const std::string stem  = srcPath.stem().string();

            // Accept only .csjsmodel (geometry) and .csjsmodelanim (animation)
            bool isModel = (ext == ".csjsmodel");
            bool isAnim  = (ext == ".csjsmodelanim");

            if (!isModel && !isAnim)
                continue; // skip unrecognised extensions

            std::string destPath = outDir + "\\" + stem + outExt;

            bool ok = false;
            if (isAnim)
                ok = convert_anim_file(srcPath.string(), destPath, flags);
            else
                ok = convert_model_file(srcPath.string(), destPath, flags);

            if (ok) ++converted; else ++errors;
        }

        time = current_time_millis() - time;
        std::cerr << "\nBatch done in " << time << " ms: "
                  << converted << " converted, "
                  << errors    << " errors." << std::endl;
        return errors > 0 ? 1 : 0;
    }

    // ── SINGLE FILE MODE ─────────────────────────────────────────────────────
    if (!fs::exists(inPath))
        return exit_with_err(inPath + " does not exist!");

    bool isAnimation = detect_is_animation(inPath);

    if (file_exists(outPath.c_str()) && !flags.count('r'))
        exit_with_err(outPath + " already exists!");

    long time = current_time_millis();

    if (isAnimation) {
        if (!convert_anim_file(inPath, outPath, flags))
            return 1;
    } else {
        if (!convert_model_file(inPath, outPath, flags))
            return 1;
    }

    time = current_time_millis() - time;
    std::cerr << "Done! (" << time << " ms)" << std::endl;
    return 0;
}

