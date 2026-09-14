#include "call_command.h"
#include "arg_coerce.h"
#include "../../string_utils.h"
#include <fmt/format.h>
#include <fstream>
#include <sstream>

std::optional<std::string> CallCommand::resolveFileParam(const std::string& param)
{
    if (!strutil::starts_with(param, '@'))
        return param;   // not a file reference — the literal value

    std::string filePath = param.substr(1);
    std::ifstream file(filePath);
    if (!file.is_open())
        return std::nullopt;   // couldn't open the file

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();   // may be "" for a genuinely empty (but readable) file
}

int CallCommand::execute(const std::vector<std::string>& args)
{
    std::string moduleName;
    std::string methodName;
    std::vector<std::string> methodArgs;

    if (args.empty()) {
        output().printError("INVALID_ARGS",
                            "Usage: logosctl call <module> <method> [args...]");
        return 1;
    }

    if (args.size() >= 3 && args[1] == "method") {
        moduleName = args[0];
        methodName = args[2];
        for (size_t j = 3; j < args.size(); ++j)
            methodArgs.push_back(args[j]);
    } else {
        if (args.size() < 2) {
            output().printError("INVALID_ARGS",
                                "Usage: logosctl call <module> <method> [args...]");
            return 1;
        }
        moduleName = args[0];
        methodName = args[1];
        for (size_t j = 2; j < args.size(); ++j)
            methodArgs.push_back(args[j]);
    }

    if (moduleName.empty() || methodName.empty()) {
        output().printError("INVALID_ARGS",
                            "Usage: logosctl call <module> <method> [args...]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    // Resolve @file parameters and coerce types to native JSON values
    LogosList resolvedArgs = LogosList::array();
    for (const std::string& arg : methodArgs) {
        // Explicit JSON argument: `json:<inline>` or `json:@<file>`. The scalar
        // coercion below can only produce bool/int/double/string, so this is the
        // way to pass a list ([T]) or map ({K:V}) — or any nested JSON value —
        // e.g. `json:[1,2,3]`, `json:{"k":"v"}`, `json:@payload.json`. This
        // mirrors the two-form convention used by jq (--arg / --argjson) and
        // HTTPie (= / :=): the default is a plain value, `json:` opts into
        // parsing. A bare `@file` (no prefix) keeps its raw-string behaviour,
        // and `str:` (below) forces a literal string.
        if (strutil::starts_with(arg, "json:")) {
            const std::string body = arg.substr(5);
            const std::optional<std::string> text = resolveFileParam(body);  // json:@file → contents; else the inline text
            if (!text) {  // json:@file whose file couldn't be opened
                output().printError("INVALID_ARGS",
                                    fmt::format("Failed to read file: {}", body.substr(1)));
                return 1;
            }
            try {
                resolvedArgs.push_back(LogosList::parse(*text));
            } catch (const std::exception& e) {
                output().printError("INVALID_ARGS",
                                    fmt::format("Invalid JSON in argument '{}': {}", arg, e.what()));
                return 1;
            }
            continue;
        }

        // Explicit literal string: `str:<text>` passes <text> verbatim, with no
        // JSON parsing, no `@file` read, and no scalar coercion. It is the
        // symmetric escape for the `json:` opt-in (mirroring jq's --arg and
        // HTTPie's `=`): it makes *every* string value expressible, including
        // ones the default path would otherwise reinterpret — `str:json:hi`
        // → the string "json:hi", `str:42` → "42", `str:@x` → "@x".
        if (strutil::starts_with(arg, "str:")) {
            resolvedArgs.push_back(arg.substr(4));
            continue;
        }

        const std::optional<std::string> resolvedOpt = resolveFileParam(arg);
        if (!resolvedOpt) {  // @file whose file couldn't be opened
            output().printError("INVALID_ARGS",
                                fmt::format("Failed to read file: {}", arg.substr(1)));
            return 1;
        }
        const std::string& resolved = *resolvedOpt;  // may be "" (empty @file)

        // The one-argument type guess, in arg_coerce.cpp so it can be pinned
        // by the unit suite on its own. `0x` hex, `inf` and `nan` are STRINGS:
        // see that file for the address that was silently becoming 7.9e+47.
        resolvedArgs.push_back(argcoerce::scalar(resolved));
    }

    LogosMap result = client().callModuleMethod(moduleName, methodName, resolvedArgs);

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        std::string code = result.value("code", std::string{});
        int exitCode = 4;
        if (code == "MODULE_NOT_LOADED" || code == "MODULE_NOT_FOUND")
            exitCode = 3;
        std::string message = result.value("message", std::string{});
        // METHOD_NOT_FOUND carries the module's real method list. JSON mode gets
        // it for free — printError merges every extra field into the envelope —
        // but the human branch prints the message and nothing else, so the list
        // has to ride along in the message to reach a person. Both forms then
        // match docs/spec.md's `call` samples.
        if (!output().isJsonMode()) {
            // find(), not operator[] — the latter would INSERT a null
            // "available_methods" into the envelope printError is about to echo.
            auto avail = result.find("available_methods");
            if (avail != result.end() && avail->is_array() && !avail->empty()) {
                std::string joined;
                for (const auto& m : *avail) {
                    if (!m.is_string()) continue;
                    if (!joined.empty()) joined += ", ";
                    joined += m.get<std::string>();
                }
                if (!joined.empty())
                    message += "\n  Available methods: " + joined;
            }
        }
        output().printError(code, message, result);
        return exitCode;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        const auto& resultValue = result["result"];
        if (resultValue.is_string()) {
            output().printRaw(resultValue.get<std::string>());
        } else if (resultValue.is_number_float()) {
            double d = resultValue.get<double>();
            if (d == static_cast<double>(static_cast<int64_t>(d)))
                output().printRaw(fmt::format("{}", static_cast<int64_t>(d)));
            else
                output().printRaw(fmt::format("{}", d));
        } else if (resultValue.is_number_unsigned()) {
            // A `uint` return above int64max is stored unsigned; reading it as
            // int64_t wraps (uint64max printed as -1). JSON mode was always
            // right — it dumps the raw number — so this only bit human mode.
            output().printRaw(fmt::format("{}", resultValue.get<uint64_t>()));
        } else if (resultValue.is_number_integer()) {
            output().printRaw(fmt::format("{}", resultValue.get<int64_t>()));
        } else if (resultValue.is_boolean()) {
            output().printRaw(resultValue.get<bool>() ? "true" : "false");
        } else if (resultValue.is_null()) {
            // Nothing useful to print
        } else {
            output().printRaw(resultValue.dump(2));
        }
    }

    return 0;
}
