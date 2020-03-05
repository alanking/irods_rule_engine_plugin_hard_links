#include <irods/filesystem/filesystem.hpp>
#include <irods/filesystem/filesystem_error.hpp>
#include <irods/irods_plugin_context.hpp>
#include <irods/irods_re_plugin.hpp>
#include <irods/irods_re_serialization.hpp>
#include <irods/irods_re_ruleexistshelper.hpp>
#include <irods/irods_get_l1desc.hpp>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_state_table.h>
#include <irods/modDataObjMeta.h>
#include <irods/msParam.h>
#include <irods/objInfo.h>
#include <irods/rcMisc.h>
#include <irods/rodsError.h>
#include <irods/rodsErrorTable.h>
#include <irods/filesystem.hpp>
#include <irods/irods_logger.hpp>
#include <irods/irods_query.hpp>
#include <irods/rsModDataObjMeta.hpp>
#include <irods/rsDataObjUnlink.hpp>
#include <irods/rsPhyPathReg.hpp>

#include "boost/any.hpp"
#include "json.hpp"

#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <iterator>
#include <functional>
#include <optional>

namespace
{
    // clang-format off
    namespace fs = irods::experimental::filesystem;

    using log    = irods::experimental::log;
    using json   = nlohmann::json;
    // clang-format on

    namespace util
    {
        auto get_rei(irods::callback& effect_handler) -> ruleExecInfo_t&
        {
            ruleExecInfo_t* rei{};
            irods::error result{effect_handler("unsafe_ms_ctx", &rei)};

            if (!result.ok()) {
                THROW(result.code(), "failed to get rule execution info");
            }

            return *rei;
        }

        // TODO Remove this.
        template <typename Function>
        auto sudo(ruleExecInfo_t& rei, Function func) -> decltype(func())
        {
            auto& auth_flag = rei.rsComm->clientUser.authInfo.authFlag;
            const auto old_auth_flag = auth_flag;

            // Elevate privileges.
            auth_flag = LOCAL_PRIV_USER_AUTH;

            // Restore authorization flags on exit.
            irods::at_scope_exit<std::function<void()>> at_scope_exit{
                [&auth_flag, old_auth_flag] { auth_flag = old_auth_flag; }
            };

            return func();
        }

        auto log_exception_message(const char* msg, irods::callback& effect_handler) -> void
        {
            log::rule_engine::error(msg);
            addRErrorMsg(&get_rei(effect_handler).rsComm->rError, RE_RUNTIME_ERROR, msg);
        }

        template <typename T>
        auto get_input_object_ptr(std::list<boost::any>& rule_arguments) -> T*
        {
            return boost::any_cast<T*>(*std::next(std::begin(rule_arguments), 2));
        }

        auto get_uuid(rsComm_t& conn, const fs::path& p) -> std::optional<std::string>
        {
            const auto gql = fmt::format("select META_DATA_ATTR_VALUE where COLL_NAME = '{}' and "
                                         "DATA_NAME = '{}' and META_DATA_ATTR_NAME = 'irods::hard_link'",
                                         p.parent_path().c_str(),
                                         p.object_name().c_str());

            for (auto&& row : irods::query{&conn, gql}) {
                return row[0];
            }

            return std::nullopt;
        }

        auto get_data_objects(rsComm_t& conn, std::string_view uuid) -> std::vector<fs::path>
        {
            const auto gql = fmt::format("select COLL_NAME, DATA_NAME where META_DATA_ATTR_NAME = 'irods::hard_link' and "
                                         "META_DATA_ATTR_VALUE = '{}'",
                                         uuid);

            std::vector<fs::path> data_objects;

            for (auto&& row : irods::query{&conn, gql}) {
                data_objects.push_back(fs::path{row[0]} / row[1]);
            }

            return data_objects;
        }

        auto get_sibling_data_objects(rsComm_t& conn, const fs::path& p) -> std::vector<fs::path>
        {
            const auto uuid = get_uuid(conn, p);

            if (!uuid) {
                return {};
            }

            auto data_objects = get_data_objects(conn, *uuid);
            auto end = std::end(data_objects);

            data_objects.erase(std::remove(std::begin(data_objects), end, p), end);

            return data_objects;
        }

        auto get_physical_path(rsComm_t& conn, const fs::path& p) -> std::string
        {
            const auto gql = fmt::format("select DATA_PATH where COLL_NAME = '{}' and DATA_NAME = '{}'",
                                         p.parent_path().c_str(),
                                         p.object_name().c_str());

            for (auto&& row : irods::query{&conn, gql}) {
                return row[0];
            }

            throw std::runtime_error{fmt::format("Could not retrieve physical path for [{}]", p.c_str())};
        }

        auto set_physical_path(rsComm_t& conn, const fs::path& logical_path, const fs::path& physical_path) -> int
        {
            dataObjInfo_t info{};
            rstrcpy(info.objPath, logical_path.c_str(), MAX_NAME_LEN);

            keyValPair_t reg_params{};
            addKeyVal(&reg_params, FILE_PATH_KW, physical_path.c_str());

            modDataObjMeta_t input{};
            input.dataObjInfo = &info;
            input.regParam = &reg_params;

            return rsModDataObjMeta(&conn, &input);
        }
    } // namespace util

    //
    // PEP Handlers
    //

    namespace handler
    {
        auto pep_api_data_obj_rename_post(std::list<boost::any>& rule_arguments, irods::callback& effect_handler) -> irods::error
        {
            try {
                auto* input = util::get_input_object_ptr<dataObjCopyInp_t>(rule_arguments);
                auto& conn = *util::get_rei(effect_handler).rsComm;
                const auto physical_path = util::get_physical_path(conn, input->destDataObjInp.objPath);

                for (auto&& sibling : util::get_sibling_data_objects(conn, input->destDataObjInp.objPath)) {
                    // TODO Check error code.
                    // Should we throw?
                    // Should this be an atomic operation?
                    // What should happen if one of many hard-links fail to be updated?
                    //
                    // From code review:
                    // - Introduce general-purpose batch/bulk catalog statements as API plugin.
                    // - Supports atomic or individual statements.
                    if (const auto ec = util::set_physical_path(conn, sibling, physical_path); ec < 0) {
                        log::rule_engine::error("Could not update physical path of [{}] to [{}]. "
                                                "Use iadmin modrepl to update remaining data objects.",
                                                input->destDataObjInp.objPath,
                                                sibling.c_str());

                        addRErrorMsg(&util::get_rei(effect_handler).rsComm->rError, RE_RUNTIME_ERROR, "");
                    }

                    //log::rule_engine::trace("Setting physical path of data object [{}] to [{}] :::: ec = {}",
                    //                        sibling.c_str(),
                    //                        physical_path, ec);
                }
            }
            catch (const std::exception& e) {
                util::log_exception_message(e.what(), effect_handler);
                return ERROR(RE_RUNTIME_ERROR, e.what());
            }

            return CODE(RULE_ENGINE_CONTINUE);
        }

        auto pep_api_data_obj_unlink_pre(std::list<boost::any>& rule_arguments, irods::callback& effect_handler) -> irods::error
        {
            // TODO Use resource id as the avu unit to identify which replicas are participating in
            // the hard-link group.

            try {
                auto* input = util::get_input_object_ptr<dataObjInp_t>(rule_arguments);
                auto& conn = *util::get_rei(effect_handler).rsComm;

                // Unregister the data object.
                // Hard-links do NOT appear in the trash.
                if (const auto uuid = util::get_uuid(conn, input->objPath); uuid && util::get_data_objects(conn, *uuid).size() > 1) {
                    log::rule_engine::trace("Removing hard-link [{}] ...", input->objPath);

                    dataObjInp_t unreg_input{};
                    unreg_input.oprType = UNREG_OPR;
                    rstrcpy(unreg_input.objPath, input->objPath, MAX_NAME_LEN);
                    addKeyVal(&unreg_input.condInput, FORCE_FLAG_KW, "");

                    if (const auto ec = rsDataObjUnlink(&conn, &unreg_input); ec < 0) {
                        log::rule_engine::error("Could not remove hard-link [{}]", input->objPath);
                        return ERROR(ec, "Hard-Link removal error");
                    }

                    log::rule_engine::trace("Successfully removed hard-link [{}]. Skipping operation.", input->objPath);

                    return CODE(RULE_ENGINE_SKIP_OPERATION);
                }

                log::rule_engine::trace("Removing data object ...");
            }
            catch (const std::exception& e) {
                util::log_exception_message(e.what(), effect_handler);
                return ERROR(RE_RUNTIME_ERROR, e.what());
            }

            return CODE(RULE_ENGINE_CONTINUE);
        }

        class pep_api_data_obj_trim final
        {
        public:
            pep_api_data_obj_trim() = delete;

            static auto pre(std::list<boost::any>& rule_arguments, irods::callback& effect_handler) -> irods::error
            {
                // TODO Use resource id as the avu unit to identify which replicas are participating in
                // the hard-link group.

                try {
                    auto* input = util::get_input_object_ptr<dataObjInp_t>(rule_arguments);
                    auto& conn = *util::get_rei(effect_handler).rsComm;

                    // Unregister the data object.
                    // Hard-links do NOT appear in the trash.
                    if (const auto uuid = util::get_uuid(conn, input->objPath); uuid && util::get_data_objects(conn, *uuid).size() > 1) {
                        log::rule_engine::trace("Removing hard-link [{}] ...", input->objPath);

                        dataObjInp_t unreg_input{};
                        unreg_input.oprType = UNREG_OPR;
                        rstrcpy(unreg_input.objPath, input->objPath, MAX_NAME_LEN);
                        addKeyVal(&unreg_input.condInput, FORCE_FLAG_KW, "");

                        if (const auto ec = rsDataObjUnlink(&conn, &unreg_input); ec < 0) {
                            log::rule_engine::error("Could not remove hard-link [{}]", input->objPath);
                            return ERROR(ec, "Hard-Link removal error");
                        }

                        log::rule_engine::trace("Successfully removed hard-link [{}]. Skipping operation.", input->objPath);

                        return CODE(RULE_ENGINE_SKIP_OPERATION);
                    }

                    log::rule_engine::trace("Removing data object ...");
                }
                catch (const std::exception& e) {
                    util::log_exception_message(e.what(), effect_handler);
                    return ERROR(RE_RUNTIME_ERROR, e.what());
                }

                return CODE(RULE_ENGINE_CONTINUE);
            }

            static auto post(std::list<boost::any>& rule_arguments, irods::callback& effect_handler) -> irods::error
            {
                return CODE(RULE_ENGINE_CONTINUE);
            }

        private:
        }; // class pep_api_data_obj_trim

        auto make_hard_link(std::list<boost::any>& rule_arguments, irods::callback& effect_handler) -> irods::error
        {
            // Inputs:
            // - Physical path
            // - Unique logical path
            // - UUID
            //
            // 1. Register the physical path as the logical path.
            // 2. Generate a catalog-unique UUID.
            // 3. Attach the UUID to the new logical path.

            try {
                auto args_iter = std::begin(rule_arguments);
                const auto logical_path = boost::any_cast<std::string>(*args_iter);
                const auto link_name = boost::any_cast<std::string>(*++args_iter);
                auto& conn = *util::get_rei(effect_handler).rsComm;

                const auto physical_path = util::get_physical_path(conn, logical_path);

                dataObjInp_t input{};
                addKeyVal(&input.condInput, FILE_PATH_KW, physical_path.data());
                rstrcpy(input.objPath, link_name.data(), MAX_NAME_LEN);

                if (const auto ec = rsPhyPathReg(&conn, &input); ec < 0) {
                    log::rule_engine::error("Could not make hard-link [ec = {}, physical_path = {}, link_name = {}]", ec, physical_path, link_name);
                }

                log::rule_engine::trace("Successfully registered data object [logical_path = {}, physical_path = {}]", logical_path.data(), physical_path.data());

                const auto uuid = [&conn, &logical_path] {
                    // If a UUID has already been assigned to the source logical path, then return that.
                    if (const auto uuid = util::get_uuid(conn, logical_path); uuid) {
                        return std::make_tuple(false, *uuid);
                    }

                    // Generate an unused UUID and return it.
                    auto uuid = to_string(boost::uuids::random_generator{}());
                    auto gql = fmt::format("select COUNT(DATA_NAME) where META_DATA_ATTR_NAME = 'irods::hard_link' and META_DATA_ATTR_VALUE = '{}'", uuid);

                    for (auto&& row : irods::query{&conn, gql}) {
                        log::rule_engine::trace("UUID [{}] already in use. Generating new UUID ...", uuid);
                        uuid = to_string(boost::uuids::random_generator{}());
                        gql = fmt::format("select COUNT(DATA_NAME) where META_DATA_ATTR_NAME = 'irods::hard_link' and META_DATA_ATTR_VALUE = '{}'", uuid);
                    }

                    return std::make_tuple(true, uuid);
                }();

                // Get the resource id of the source logical path.
                const auto resc_id = [&conn, p = fs::path{logical_path}] {
                    const auto gql = fmt::format("select RESC_ID where COLL_NAME = '{}' and DATA_NAME = '{}'",
                                                 p.parent_path().c_str(),
                                                 p.object_name().c_str());

                    for (auto&& row : irods::query{&conn, gql}) {
                        return row[0];
                    }

                    THROW(SYS_INTERNAL_ERR, "Could not get resource id for source logical path");
                }();

                try {
                    fs::server::set_metadata(conn, link_name, {"irods::hard_link", std::get<std::string>(uuid), resc_id});

                    if (const auto& [new_uuid, uuid_value] = uuid; new_uuid) {
                        fs::server::set_metadata(conn, logical_path, {"irods::hard_link", uuid_value, resc_id});
                    }
                }
                catch (const fs::filesystem_error& e) {
                    log::rule_engine::error("Could not set hard-link metadata [msg = {}, ec = {}]", e.what(), e.code().value());
                    return ERROR(e.code().value(), e.what());
                }
            }
            catch (const std::exception& e) {
                util::log_exception_message(e.what(), effect_handler);
                return ERROR(RE_RUNTIME_ERROR, e.what());
            }

            return SUCCESS();
        }
    } // namespace handler

    //
    // Rule Engine Plugin
    //

    // clang-format off
    using handler_type     = std::function<irods::error(std::list<boost::any>&, irods::callback&)>;
    using handler_map_type = std::map<std::string_view, handler_type>;

    const handler_map_type pep_handlers{
        {"pep_api_data_obj_rename_post", handler::pep_api_data_obj_rename_post},
        {"pep_api_data_obj_unlink_pre",  handler::pep_api_data_obj_unlink_pre},
        {"pep_api_data_obj_trim_post",   handler::pep_api_data_obj_trim::post},
        {"pep_api_data_obj_trim_pre",    handler::pep_api_data_obj_trim::pre}
    };

    // TODO Could expose these as a new .so. The .so would then be loaded by the new "irods" cli.
    // Then we get things like: irods ln <args>...
    const handler_map_type hard_link_handlers{
        {"hard_links_count_links", {}},
        {"hard_links_list_data_objects", {}},
        {"hard_links_make_link", handler::make_hard_link}
    };
    // clang-format on

    template <typename ...Args>
    using operation = std::function<irods::error(irods::default_re_ctx&, Args...)>;

    auto rule_exists(irods::default_re_ctx&, const std::string& rule_name, bool& exists) -> irods::error
    {
        exists = pep_handlers.find(rule_name) != std::end(pep_handlers);
        return SUCCESS();
    }

    auto list_rules(irods::default_re_ctx&, std::vector<std::string>& rules) -> irods::error
    {
        std::transform(std::begin(hard_link_handlers),
                       std::end(hard_link_handlers),
                       std::back_inserter(rules),
                       [](auto v) { return std::string{v.first}; });

        std::transform(std::begin(pep_handlers),
                       std::end(pep_handlers),
                       std::back_inserter(rules),
                       [](auto v) { return std::string{v.first}; });

        return SUCCESS();
    }

    auto exec_rule(irods::default_re_ctx&,
                   const std::string& rule_name,
                   std::list<boost::any>& rule_arguments,
                   irods::callback effect_handler) -> irods::error
    {
        if (auto iter = pep_handlers.find(rule_name); std::end(pep_handlers) != iter) {
            return (iter->second)(rule_arguments, effect_handler);
        }

        log::rule_engine::error("Rule not supported [{}]", rule_name);

        return CODE(RULE_ENGINE_CONTINUE);
    }

    auto exec_rule_text_impl(std::string_view rule_text, irods::callback effect_handler) -> irods::error
    {
        log::rule_engine::debug({{"rule_text", std::string{rule_text}}});

        // irule <text>
        if (rule_text.find("@external rule {") != std::string::npos) {
            const auto start = rule_text.find_first_of('{') + 1;
            rule_text = rule_text.substr(start, rule_text.rfind(" }") - start);
        }
        // irule -F <script>
        else if (rule_text.find("@external") != std::string::npos) {
            const auto start = rule_text.find_first_of('{');
            rule_text = rule_text.substr(start, rule_text.rfind(" }") - start);
        }

        log::rule_engine::debug({{"rule_text", std::string{rule_text}}});

        try {
            const auto json_args = json::parse(rule_text);

            log::rule_engine::debug({{"function", __func__}, {"json_arguments", json_args.dump()}});

            const auto op = json_args.at("operation").get<std::string>();

            if (const auto iter = hard_link_handlers.find(op); iter != std::end(hard_link_handlers)) {
                std::list<boost::any> args{
                    json_args.at("logical_path").get<std::string>(),
                    json_args.at("link_name").get<std::string>()
                };

                return (iter->second)(args, effect_handler);
            }

            return ERROR(INVALID_OPERATION, fmt::format("Invalid operation [{}]", op));
        }
        catch (const json::parse_error& e) {
            // clang-format off
            log::rule_engine::error({{"rule_engine_plugin", "hard_links"},
                                     {"rule_engine_plugin_function", __func__},
                                     {"log_message", e.what()}});
            // clang-format on

            return ERROR(USER_INPUT_FORMAT_ERR, e.what());
        }
        catch (const json::type_error& e) {
            // clang-format off
            log::rule_engine::error({{"rule_engine_plugin", "hard_links"},
                                     {"rule_engine_plugin_function", __func__},
                                     {"log_message", e.what()}});
            // clang-format on

            return ERROR(SYS_INTERNAL_ERR, e.what());
        }
        catch (const std::exception& e) {
            // clang-format off
            log::rule_engine::error({{"rule_engine_plugin", "hard_links"},
                                     {"rule_engine_plugin_function", __func__},
                                     {"log_message", e.what()}});
            // clang-format on

            return ERROR(SYS_INTERNAL_ERR, e.what());
        }
        catch (...) {
            // clang-format off
            log::rule_engine::error({{"rule_engine_plugin", "hard_links"},
                                     {"rule_engine_plugin_function", __func__},
                                     {"log_message", "Unknown error"}});
            // clang-format on

            return ERROR(SYS_UNKNOWN_ERROR, "Unknown error");
        }
    }
} // namespace (anonymous)

//
// Plugin Factory
//

using pluggable_rule_engine = irods::pluggable_rule_engine<irods::default_re_ctx>;

extern "C"
auto plugin_factory(const std::string& _instance_name,
                    const std::string& _context) -> pluggable_rule_engine*
{
    const auto no_op = [](auto&&...) { return SUCCESS(); };

    const auto exec_rule_text_wrapper = [](irods::default_re_ctx&,
                                                 const std::string& rule_text,
                                                 msParamArray_t*,
                                                 const std::string&,
                                                 irods::callback effect_handler)
    {
        return exec_rule_text_impl(rule_text, effect_handler);
    };

    const auto exec_rule_expression_wrapper = [](irods::default_re_ctx&,
                                                 const std::string& rule_text,
                                                 msParamArray_t* ms_params,
                                                 irods::callback effect_handler)
    {
        return exec_rule_text_impl(rule_text, effect_handler);
    };

    auto* re = new pluggable_rule_engine{_instance_name, _context};

    re->add_operation("start", operation<const std::string&>{no_op});
    re->add_operation("stop", operation<const std::string&>{no_op});
    re->add_operation("rule_exists", operation<const std::string&, bool&>{rule_exists});
    re->add_operation("list_rules", operation<std::vector<std::string>&>{list_rules});
    re->add_operation("exec_rule", operation<const std::string&, std::list<boost::any>&, irods::callback>{exec_rule});
    re->add_operation("exec_rule_text", operation<const std::string&, msParamArray_t*, const std::string&, irods::callback>{exec_rule_text_wrapper});
    re->add_operation("exec_rule_expression", operation<const std::string&, msParamArray_t*, irods::callback>{exec_rule_expression_wrapper});

    return re;
}

