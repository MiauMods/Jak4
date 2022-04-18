#include "third-party/CLI11.hpp"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "decompiler/Disasm/OpcodeInfo.h"
#include "decompiler/ObjectFile/ObjectFileDB.h"
#include "decompiler/level_extractor/extract_level.h"
#include "decompiler/config.h"
#include "goalc/compiler/Compiler.h"
#include "common/util/read_iso_file.h"

void setup_global_decompiler_stuff(std::optional<std::filesystem::path> project_path_override) {
  decompiler::init_opcode_info();
  file_util::setup_project_path(project_path_override);
}

void extract_files(std::filesystem::path data_dir_path, std::filesystem::path extracted_iso_path) {
  fmt::print("Note: input isn't a folder, assuming it's an ISO file...\n");

  std::filesystem::create_directories(extracted_iso_path);

  auto fp = fopen(data_dir_path.string().c_str(), "rb");
  ASSERT_MSG(fp, "failed to open input ISO file\n");
  unpack_iso_files(fp, extracted_iso_path);
  fclose(fp);
}

int validate(std::filesystem::path path_to_iso_files) {
  if (!std::filesystem::exists(path_to_iso_files / "DGO")) {
    fmt::print("Error: input folder doesn't have a DGO folder. Is this the right input?\n");
    return 1;
  }
  return 0;
}

void decompile(std::filesystem::path jak1_input_files) {
  using namespace decompiler;
  Config config = read_config_file(
      (file_util::get_jak_project_dir() / "decompiler" / "config" / "jak1_ntsc_black_label.jsonc")
          .string(),
      {});

  std::vector<std::string> dgos, objs;

  // grab all DGOS we need (level + common)
  for (const auto& dgo_name : config.dgo_names) {
    std::string common_name = "GAME.CGO";
    if (dgo_name.length() > 3 && dgo_name.substr(dgo_name.length() - 3) == "DGO") {
      // ends in DGO, it's a level
      dgos.push_back((jak1_input_files / dgo_name).string());
    } else if (dgo_name.length() >= common_name.length() &&
               dgo_name.substr(dgo_name.length() - common_name.length()) == common_name) {
      // it's COMMON.CGO, we need that too.
      dgos.push_back((jak1_input_files / dgo_name).string());
    }
  }

  // grab all the object files we need (just text)
  for (const auto& obj_name : config.object_file_names) {
    if (obj_name.length() > 3 && obj_name.substr(obj_name.length() - 3) == "TXT") {
      // ends in DGO, it's a level
      objs.push_back((jak1_input_files / obj_name).string());
    }
  }

  // set up objects
  ObjectFileDB db(dgos, config.obj_file_name_map_file, objs, {}, config);

  // save object files
  auto out_folder = (file_util::get_jak_project_dir() / "decompiler_out" / "jak1").string();
  auto raw_obj_folder = file_util::combine_path(out_folder, "raw_obj");
  file_util::create_dir_if_needed(raw_obj_folder);
  db.dump_raw_objects(raw_obj_folder);

  // analyze object file link data
  db.process_link_data(config);
  db.find_code(config);
  db.process_labels();

  // text files
  {
    auto result = db.process_game_text_files(config);
    if (!result.empty()) {
      file_util::write_text_file(file_util::get_file_path({"assets", "game_text.txt"}), result);
    }
  }

  // textures
  decompiler::TextureDB tex_db;
  file_util::write_text_file(file_util::get_file_path({"assets", "tpage-dir.txt"}),
                             db.process_tpages(tex_db));
  // texture replacements
  auto replacements_path = file_util::get_file_path({"texture_replacements"});
  if (std::filesystem::exists(replacements_path)) {
    tex_db.replace_textures(replacements_path);
  }

  // game count
  {
    auto result = db.process_game_count_file();
    if (!result.empty()) {
      file_util::write_text_file(file_util::get_file_path({"assets", "game_count.txt"}), result);
    }
  }

  // levels
  {
    extract_all_levels(db, tex_db, config.levels_to_extract, "GAME.CGO", config.hacks,
                       config.rip_levels);
  }
}

void compile(std::filesystem::path extracted_iso_path) {
  Compiler compiler;
  compiler.make_system().set_constant("*iso-data*", absolute(extracted_iso_path).string());
  compiler.make_system().set_constant("*use-iso-data-path*", true);

  compiler.make_system().load_project_file(
      (file_util::get_jak_project_dir() / "goal_src" / "game.gp").string());
  compiler.run_front_end_on_string("(mi)");
}

void launch_game() {
  system((file_util::get_jak_project_dir() / "../gk").string().c_str());
}

int main(int argc, char** argv) {
  std::filesystem::path data_dir_path;
  std::filesystem::path project_path_override;
  bool flag_runall = false;
  bool flag_extract = false;
  bool flag_validate = false;
  bool flag_decompile = false;
  bool flag_compile = false;
  bool flag_play = false;

  lg::initialize();

  CLI::App app{"OpenGOAL Level Extraction Tool"};
  app.add_option("game-files-path", data_dir_path,
                 "The path to the folder with the ISO extracted or the ISO itself")
      ->check(CLI::ExistingPath)
      ->required();
  app.add_option("--proj-path", project_path_override,
                 "Explicitly set the location of the 'data/' folder")
      ->check(CLI::ExistingPath);
  app.add_flag("-a,--all", flag_runall, "Run all steps, from extraction to playing the game");
  app.add_flag("-e,--extract", flag_extract, "Extract the ISO");
  app.add_flag("-v,--validate", flag_validate, "Validate the ISO / game files");
  app.add_flag("-d,--decompile", flag_decompile, "Decompile the game data");
  app.add_flag("-c,--compile", flag_compile, "Compile the game");
  app.add_flag("-p,--play", flag_play, "Play the game");
  app.validate_positionals();

  CLI11_PARSE(app, argc, argv);

  fmt::print("Working Directory - {}\n", std::filesystem::current_path().string());

  // If no flag is set, we default to running everything
  if (!flag_extract && !flag_validate && !flag_decompile && !flag_compile && !flag_play) {
    fmt::print("Running all steps, no flags provided!\n");
    flag_runall = true;
  }

  // todo: print revision here.
  if (!project_path_override.empty()) {
    setup_global_decompiler_stuff(std::make_optional(project_path_override));
  } else {
    setup_global_decompiler_stuff(std::nullopt);
  }

  auto path_to_iso_files = file_util::get_jak_project_dir() / "extracted_iso";

  // make sure the input looks right
  if (!std::filesystem::exists(data_dir_path)) {
    fmt::print("Error: input folder {} does not exist\n", data_dir_path.string());
    return 1;
  }

  if (flag_runall || flag_extract) {
    if (!std::filesystem::is_directory(path_to_iso_files)) {
      extract_files(data_dir_path, path_to_iso_files);
    }
  }

  if (flag_runall || flag_validate) {
    auto ok = validate(path_to_iso_files);
    if (ok != 0) {
      return ok;
    }
  }

  if (flag_runall || flag_decompile) {
    decompile(path_to_iso_files);
  }

  if (flag_runall || flag_compile) {
    compile(path_to_iso_files);
  }

  if (flag_runall || flag_play) {
    launch_game();
  }

  return 0;
}
