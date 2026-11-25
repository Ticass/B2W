// See clientport.h.
#include "clientport.h"

#include "core/files.h"
#include "core/regex.h"
#include "rewrites.h"
#include "vm_rules.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace w2b::gsc::port::clientport
{
    namespace fs = std::filesystem;

    namespace
    {
        const std::map<std::string, std::string> CLIENT_RUNTIME{
            {"clientscripts\\_load", "BO2's clientscripts\\mp\\zombies\\_load starts the client VM"},
            {"clientscripts\\_callbacks", "engine callbacks are answered by BO2's clientscripts\\mp\\_callbacks"},
        };
        const std::map<std::pair<std::string, std::string>, std::string> CLIENT_MODULE_FUNCTIONS{{{"clientscripts\\_audio", "playloopat"}, "waw_playloopat"}};
        const std::map<std::string, std::string> CLIENT_METHODS{
            {"setlightintensity", "waw_setlightintensity"}, {"getlightintensity", "waw_getlightintensity"}, {"haseyes", "waw_haseyes"}, {"delete", "waw_client_delete"}};
        const std::map<std::string, std::string> CLIENT_BUILTINS{{"playsound", "waw_client_playsound"},
                                                                 {"setvolfog", "waw_client_setvolfog"},
                                                                 {"syncsystemstates", "waw_syncsystemstates"},
                                                                 {"updatedvarsfromprofile", "waw_updatedvarsfromprofile"},
                                                                 {"getsoundvolume", "waw_getsoundvolume"},
                                                                 {"getplayers", "waw_client_getplayers"},
                                                                 {"setclientdvar", "waw_setclientdvar"},
                                                                 {"prof_begin", "waw_prof"},
                                                                 {"prof_end", "waw_prof"},
                                                                 {"spawnfx", "waw_spawnfx"},
                                                                 {"triggerfx", "waw_triggerfx"}};
        const regex::Regex& IncludeRe()
        {
            static const regex::Regex include(R"(#include\s+([\w\\/]+)\s*;?)", regex::IGNORECASE);
            return include;
        }

        std::string Inner(const std::string& text)
        {
            return text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string();
        }

        std::string Ported(const std::string& path)
        {
            return PORTED_PREFIX + path.substr(std::string(CLIENT_PREFIX).size());
        }

        std::set<std::string> ClientDeps(const Script& script)
        {
            std::set<std::string> deps;
            for (const auto& p : script.includes)
                deps.insert(ClientNorm(p));
            for (const auto& r : References(script.tokens))
                if (r.qualifier)
                    deps.insert(ClientNorm(*r.qualifier));
            std::set<std::string> out;
            for (const auto& d : deps)
                if (StartsWith(d, CLIENT_PREFIX) && !StartsWith(d, "clientscripts\\mp\\"))
                    out.insert(d);
            return out;
        }

        std::pair<long long, long long> Statement(const Tokens& tokens, const long long index)
        {
            long long start = index;
            while (start > 0 && !In(tokens[start - 1].text, {";", "{", "}"}))
                start--;
            long long end = index;
            while (At(tokens, end).text != ";")
                end++;
            return {start, end};
        }

        bool InList(const util::Json& list, const std::string& value)
        {
            for (const auto& v : list)
                if (v.get<std::string>() == value)
                    return true;
            return false;
        }

        bool LookupKey(const Tokens& tokens, const long long index)
        {
            int depth = 0;
            for (long long j = index - 1; j >= 0; j--)
            {
                const auto& text = tokens[j].text;
                if (In(text, {")", "]"}))
                    depth++;
                else if (In(text, {"(", "["}))
                {
                    if (depth == 0)
                        return text == "(" && j > 0 && In(tokens[j - 1].Low(), {"getstruct", "getstructarray", "getent", "getentarray"});
                    depth--;
                }
                else if (In(text, {";", "{", "}"}))
                    return false;
            }
            return false;
        }

        void BindLiteral(Token& token, const std::map<std::string, std::string>& effects, const std::map<std::string, std::string>& weapons,
                         const std::set<std::string>& soundNames)
        {
            const auto name = Inner(token.text);
            if (const auto e = effects.find(name); e != effects.end())
                token.text = "\"" + e->second + "\"";
            else if (const auto w = weapons.find(name); w != weapons.end() && !w->second.empty())
                token.text = "\"" + w->second + "\"";
            else if (soundNames.count(name))
                token.text = "\"waw/" + name + "\"";
        }
    } // namespace

    std::string ClientNorm(const std::string& path)
    {
        auto p = Lower(Replace(path, "/", "\\"));
        if (EndsWith(p, ".csc"))
            p.resize(p.size() - 4);
        return p;
    }

    std::string TranslateLibrary(const std::string& source, const std::map<std::string, std::string>& effects,
                                 const std::map<std::string, std::string>& weapons, const std::set<std::string>& soundNames)
    {
        auto tokens = Tokenize(source);
        for (auto& token : tokens)
        {
            if (token.kind == Kind::Ident && token.Low() == "realwait")
                token.text = "waitrealtime";
            else if (token.kind == Kind::String)
                BindLiteral(token, effects, weapons, soundNames);
        }
        auto text = Emit(tokens);
        for (const char* name : {"_utility", "_fx", "_music"})
            text = Replace(text, std::string("clientscripts\\") + name, std::string("clientscripts\\mp\\") + name);
        auto script = Parse(text, "<source client library>");
        std::set<std::string> events;
        const auto& toks = script.tokens;
        for (long long i = 0; i < static_cast<long long>(toks.size()) - 4; i++)
            if (toks[i].Low() == "waittill" && i && toks[i - 1].Low() == "level" && toks[i + 1].text == "(" && toks[i + 2].kind == Kind::String && toks[i + 3].text == ")")
                events.insert(Inner(toks[i + 2].text));
        if (!events.empty())
        {
            const Function* init = script.Find("init");
            if (!init)
                throw std::runtime_error("KeyError: 'init'");
            size_t i = 0;
            std::string registrations, handlers;
            for (const auto& name : events)
            {
                registrations += "\n    " + std::string(ENVIRONMENT) + "::waw_register_system( \"" + name + "\", ::waw_notify_" + std::to_string(i) + " );";
                handlers += "\nwaw_notify_" + std::to_string(i) + "( localclientnum, state )\n{\n    level notify( \"" + name + "\" );\n}\n";
                i++;
            }
            script.tokens[init->bodyOpen].text += registrations;
            text = Emit(script.tokens) + handlers;
        }
        return text;
    }

    std::string StageTesla(const std::vector<fs::path>& roots, const fs::path& project, const std::map<std::string, std::string>& effects,
                           const std::map<std::string, std::string>& weaponTable, const std::set<std::string>& soundNames)
    {
        const auto core = project / "maps/mp/waw/_waw2bo2_core.gsc";
        std::error_code error;
        if (!fs::is_regular_file(core, error) || !Contains(files::ReadText(core), "waw_tesla_init()"))
            return "";
        std::optional<fs::path> source;
        for (const auto& root : roots)
            if (fs::is_regular_file(root / "clientscripts/_zombiemode_tesla.csc", error))
            {
                source = root / "clientscripts/_zombiemode_tesla.csc";
                break;
            }
        if (!source)
        {
            std::fprintf(stderr, "RuntimeWarning: source Tesla gameplay library has no matching client library; client effects were not staged\n");
            return "";
        }
        files::WriteText(project / "clientscripts/mp/waw/_zombiemode_tesla.csc", TranslateLibrary(files::ReadText(*source), effects, weaponTable, soundNames));
        return "    clientscripts\\mp\\waw\\_zombiemode_tesla::init();\n";
    }

    std::map<std::string, std::string> StagedClientModules(const std::string&, const std::string&)
    {
        return {{"clientscripts\\_zombiemode_tesla", "clientscripts\\mp\\waw\\_zombiemode_tesla"},
                {"clientscripts\\_audio", "clientscripts\\mp\\_audio"},
                {"clientscripts\\_ambientpackage", "clientscripts\\mp\\_ambientpackage"}};
    }

    util::Json PortMapClient(Sources& sources, const std::string& mapName, const fs::path& outRoot, const std::map<std::string, std::string>& staged,
                           const std::set<std::string>& builtins, const std::map<std::string, std::set<std::string>>& declared)
    {
        const auto mainPath = CLIENT_PREFIX + Lower(mapName);
        if (!sources.Get(mainPath, ".csc"))
            return util::Json::object();
        util::Json report = util::Json::object();
        report["entry"] = Ported(mainPath);
        for (const char* key : {"modules", "events", "fx", "dropped", "missing", "runtime", "compat", "renamed"})
            report[key] = util::Json::array();
        std::set<std::string> skip;
        for (const auto& [k, _] : CLIENT_RUNTIME)
            skip.insert(k);
        for (const auto& [k, _] : staged)
            skip.insert(k);
        std::vector<std::string> order, queue{mainPath};
        std::set<std::string> seen;
        while (!queue.empty())
        {
            const auto path = queue.front();
            queue.erase(queue.begin());
            if (seen.count(path))
                continue;
            seen.insert(path);
            const auto script = sources.Get(path, ".csc");
            if (!script)
            {
                report["missing"].push_back(path);
                continue;
            }
            order.push_back(path);
            for (const auto& d : ClientDeps(*script))
                if (!skip.count(d) && !seen.count(d))
                    queue.push_back(d);
        }
        const auto target = [&](const std::string& raw)
        {
            const auto path = ClientNorm(raw);
            if (!StartsWith(path, CLIENT_PREFIX) || StartsWith(path, "clientscripts\\mp\\"))
                return path;
            if (const auto s = staged.find(path); s != staged.end())
                return s->second;
            if (CLIENT_RUNTIME.count(path) || InList(report["missing"], path))
            {
                if (!InList(report["runtime"], path))
                    report["runtime"].push_back(path);
                return "clientscripts\\mp\\" + path.substr(std::string(CLIENT_PREFIX).size());
            }
            return Ported(path);
        };
        std::map<std::string, std::set<std::string>> defined;
        std::set<std::string> shadowed;
        for (const auto& path : order)
            for (const auto& [name, _] : sources.Get(path, ".csc")->functions)
            {
                defined[path].insert(name);
                if (builtins.count(name))
                    shadowed.insert(name);
            }
        std::set<std::string> events;
        VmCounts vm;
        for (const auto& path : order)
        {
            const auto script = sources.Get(path, ".csc");
            auto tokens = Tokenize(Emit(script->tokens));
            WrapIntegerLiterals(tokens);
            const auto refs = References(tokens);
            const Function* main = path == mainPath ? script->Find("main") : nullptr;
            std::set<std::pair<long long, long long>> statements;
            for (const auto& ref : refs)
            {
                if (!main || !(main->bodyOpen < ref.index && ref.index < main->bodyClose) || ref.pointer)
                    continue;
                const auto qualifier = ref.qualifier ? std::optional<std::string>(ClientNorm(*ref.qualifier)) : std::nullopt;
                if ((qualifier && skip.count(*qualifier)) || (!qualifier && Lower(ref.name) == "waitforclient"))
                    statements.insert(Statement(tokens, static_cast<long long>(ref.qualIndex ? *ref.qualIndex : ref.index)));
            }
            const Function* ownMain = script->Find("main");
            const auto made = declared.find(path);
            if (made != declared.end() && !made->second.empty() && ownMain)
            {
                int count = 0;
                for (const auto& ref : refs)
                    if (ownMain->bodyOpen < ref.index && ref.index < ownMain->bodyClose && !ref.qualifier && !ref.pointer && !ref.method
                        && made->second.count(Lower(ref.name)))
                    {
                        const auto [start, end] = Statement(tokens, static_cast<long long>(ref.index));
                        for (long long k = start; k < end + 1; k++)
                            tokens[k].text = "";
                        count++;
                    }
                tokens[ownMain->bodyOpen].text += "\n\t// waw2bo2: the " + std::to_string(count)
                                                  + " room, package and music declarations run from the generated _amb script on BO2's audio runtime";
                if (!report.contains("declared"))
                    report["declared"] = util::Json::object();
                report["declared"][path] = count;
            }
            for (const auto& ref : refs)
            {
                if (!ref.qualifier)
                    continue;
                const auto key = std::make_pair(ClientNorm(*ref.qualifier), Lower(ref.name));
                const auto f = CLIENT_MODULE_FUNCTIONS.find(key);
                if (f != CLIENT_MODULE_FUNCTIONS.end() && !tokens[ref.index].text.empty() && !ref.pointer)
                {
                    tokens[*ref.qualIndex].text = ENVIRONMENT;
                    tokens[ref.index].text = f->second;
                    if (!InList(report["compat"], key.second))
                        report["compat"].push_back(key.second);
                }
            }
            // the port shares BO2's system table: WaW's script cap of 32 would count BO2's systems too
            if (const Function* add = script->Find("registersystem"))
                for (auto k = add->bodyOpen; k + 3 < add->bodyClose; k++)
                    if (tokens[k].Low() == "size" && tokens[k + 1].text == ">=" && tokens[k + 2].text == "32")
                    {
                        tokens[k + 2].text = "1024";
                        report["system_limit_lifted"] = path;
                    }
            for (const auto& [start, end] : statements)
            {
                std::vector<std::string> words;
                for (long long k = start; k < end + 1; k++)
                    words.push_back(tokens[k].text);
                const auto dropped = Join(words, " ");
                report["dropped"].push_back(dropped);
                tokens[start].pre += "// waw2bo2: the BO2 client main or a separate port runs this: " + dropped + "\n\t";
                for (long long k = start; k < end + 1; k++)
                    tokens[k].text = "";
            }
            std::set<std::string> visible = defined[path];
            for (const auto& inc : script->includes)
                if (const auto d = defined.find(ClientNorm(inc)); d != defined.end())
                    visible.insert(d->second.begin(), d->second.end());
            for (const auto& ref : refs)
            {
                auto& t = tokens[ref.index];
                const auto low = Lower(ref.name);
                std::set<std::string> none;
                const std::set<std::string>* owner = &visible;
                if (ref.qualifier)
                {
                    const auto d = defined.find(ClientNorm(*ref.qualifier));
                    owner = d == defined.end() ? &none : &d->second;
                }
                if (!t.text.empty() && shadowed.count(low) && owner->count(low) && !ref.method)
                {
                    t.text = "waw_" + t.text;
                    if (!InList(report["renamed"], low))
                        report["renamed"].push_back(low);
                }
            }
            for (const auto& ref : refs)
            {
                auto& t = tokens[ref.index];
                if (t.text.empty() || ref.qualifier || ref.pointer || StartsWith(t.text, "waw_"))
                    continue;
                const auto low = Lower(ref.name);
                if (const auto m = CLIENT_METHODS.find(low); ref.method && m != CLIENT_METHODS.end())
                    t.text = std::string(ENVIRONMENT) + "::" + m->second;
                else if (const auto b = CLIENT_BUILTINS.find(low); !ref.method && b != CLIENT_BUILTINS.end() && !script->Find(low))
                    t.text = std::string(ENVIRONMENT) + "::" + b->second;
                else
                    continue;
                if (!InList(report["compat"], low))
                    report["compat"].push_back(low);
            }
            for (long long i = 0; i < static_cast<long long>(tokens.size()); i++)
            {
                auto& t = tokens[i];
                if (t.kind == Kind::Path && StartsWith(ClientNorm(t.text), CLIENT_PREFIX))
                    t.text = target(t.text);
                else if (t.kind == Kind::Directive)
                {
                    if (const auto m = IncludeRe().MatchAt(t.text))
                        t.text = "#include " + target((*m)[1]) + ";";
                }
                else if (t.Low() == "loadfx" && At(tokens, i + 1).text == "(" && At(tokens, i + 2).kind == Kind::String)
                    report["fx"].push_back(Inner(At(tokens, i + 2).text));
                else if (t.Low() == "waittill" && i > 1 && tokens[i - 1].Low() == "level" && At(tokens, i + 1).text == "(" && At(tokens, i + 2).kind == Kind::String)
                    events.insert(Inner(At(tokens, i + 2).text));
            }
            // the passes above edit text in place; the VM rules read it as tokens again
            auto ported = Tokenize(Emit(tokens));
            try
            {
                vm.Add(ApplyVmRules(ported, VmRuntime{ENVIRONMENT}));
            }
            catch (const GscSyntaxError& error)
            {
                report["vm_rules_skipped"].push_back(path + ": " + error.what());
                ported = Tokenize(Emit(tokens));
            }
            files::WriteText(outRoot / files::FromUtf8(Replace(Ported(path), "\\", "/") + ".csc"), std::string(PORT_HEADER) + " Source: " + path + ".csc\n" + Emit(ported));
            report["modules"].push_back(Ported(path));
        }
        report["events"] = std::vector<std::string>(events.begin(), events.end());
        report["vm_rules"] = {{"loops_watched", vm.watched}, {"loops", vm.loops}, {"switches", vm.switches}, {"vectors", vm.vectors},
                              {"logic_values", vm.logic}, {"array_comparisons", vm.arrays}, {"string_comparisons", vm.comparisons}};
        std::set<std::string> fx;
        for (const auto& f : report["fx"])
            fx.insert(f.get<std::string>());
        report["fx"] = std::vector<std::string>(fx.begin(), fx.end());
        return report;
    }

    namespace
    {
        constexpr std::string_view CLIENT_GLUE_SOURCE = R"GSC( T6 glue around the map's own client scripts.
#include clientscripts\mp\_utility;

init()
{
    // WaW's _load answered a server clientnotify through its levelNotify
    // system with a level notify carrying the local client. Compat
    // waw_clientnotify sends every message through BO2's levelNotify system,
    // the path measured to reach the client (2026-11-10 probe: "pac0"/"tp0"
    // arrived, "pl1"/"t2d" never came as systems of their own). The named
    // systems stay registered; waw_deliver takes whichever arrives first.
@FORWARDS@    forward_notify( "levelNotify", ::waw_level_notify );
    @ENTRY@::main();
    // BO2's connect path (zombies\_players::on_connect) runs its own
    // _fx::fx_init before level.onplayerconnect: its light mixer would take
    // the map's lights and its createfx playback would double the map's
    // effects. Both are handed to the map's own _fx at the same connect.
    level.waw_createfxent = level.createfxent;
    level.createfxent = undefined;
    // The map's light entities (classname "light", "pl#" -> .pl), collected
    // as WaW's _utility_code::struct_class_init indexes them by classname.
    level.waw_light_structs = [];
    for ( i = 0; i < level.struct.size; i++ )
    {
        if ( isdefined( level.struct[i].classname ) && level.struct[i].classname == "light" )
            level.waw_light_structs[level.waw_light_structs.size] = level.struct[i];
    }
    if ( isdefined( level.struct_class_names ) )
        level.struct_class_names["classname"]["light"] = undefined;
    level.waw_client_connect = level.onplayerconnect;
    level.onplayerconnect = ::on_connect;
    if ( getdvar( "waw_client_sound_debug" ) == "1" )
        println( "WAW CLIENT GLUE ready, createfx " + isdefined( level.waw_createfxent ) );
    // The map's main can wait (its own client modules do), and a local client
    // that connected meanwhile has already been through BO2's connect path
    // (zombies\_players::on_connect) without this hook: its lights and
    // createfx would never start (2026-11-10, no fires or lights).
    players = getlocalplayers();
    for ( i = 0; i < players.size; i++ )
        level thread on_connect( i, 1 );
}

// What clientscripts\mp\_utility::registersystem stores, without its
// script-side limit of 32 systems (BO2's own systems already use many).
forward_notify( name, callback )
{
    if ( !isdefined( level._systemstates ) )
        level._systemstates = [];
    if ( !isdefined( level._systemstates[name] ) )
        level._systemstates[name] = spawnstruct();
    level._systemstates[name].callback = callback;
    if ( !isdefined( level.waw_forwarded ) )
        level.waw_forwarded = [];
    level.waw_forwarded[name] = 1;
}

waw_level_notify( localclientnum, state )
{
    if ( getdvar( "waw_client_sound_debug" ) == "1" && isdefined( state ) )
        println( "WAW CLIENT LEVELNOTIFY " + state );
    if ( isdefined( state ) && state != "" )
        waw_deliver( state, localclientnum );
}

// One level notify per message even when both of its paths deliver it.
waw_deliver( state, localclientnum )
{
    if ( !isdefined( level.waw_delivered ) )
        level.waw_delivered = [];
    now = getrealtime();
    if ( isdefined( level.waw_delivered[state] ) && now - level.waw_delivered[state] < 100 )
        return;
    level.waw_delivered[state] = now;
    if ( getdvar( "waw_client_sound_debug" ) == "1" )
        println( "WAW CLIENT DELIVER " + state );
    level notify( state, localclientnum );
}

// late: the client connected before init installed this (BO2 already ran
// the connect callback it replaced). Once per local client either way.
on_connect( localclientnum, late )
{
    if ( !isdefined( level.waw_connected ) )
        level.waw_connected = [];
    if ( isdefined( level.waw_connected[localclientnum] ) )
        return;
    level.waw_connected[localclientnum] = 1;
    if ( getdvar( "waw_client_sound_debug" ) == "1" )
        println( "WAW CLIENT CONNECT " + localclientnum + " late " + isdefined( late ) );
    if ( isdefined( level.waw_client_connect ) && !isdefined( late ) )
        level thread [[ level.waw_client_connect ]]( localclientnum );
    if ( isdefined( level.waw_createfxent ) )
        level.createfxent = level.waw_createfxent;
    // No allocatemixerlights: BO2's mixer then owns every light's colour, and
    // WaW's lights are never made mixer lights (they drew with colour 0).
    if ( level.waw_light_structs.size )
        level.struct_class_names["classname"]["light"] = level.waw_light_structs;
    // WaW _callbacks::localclientconnect -> _load effects_init -> _fx::fx_init
@CONNECT@}
@HANDLERS@)GSC";
    } // namespace

    std::string FinishMapClient(const fs::path& projectRoot, util::Json& report, const std::map<std::string, std::string>& effects,
                                const std::map<std::string, std::string>& weapons, const std::set<std::string>& soundNames)
    {
        std::set<std::string> unbound;
        if (report.contains("fx"))
            for (const auto& f : report["fx"])
                if (!effects.count(f.get<std::string>()))
                    unbound.insert(f.get<std::string>());
        report["unbound_fx"] = std::vector<std::string>(unbound.begin(), unbound.end());
        const auto modules = report.contains("modules") ? report["modules"] : util::Json::array();
        for (const auto& module : modules)
        {
            const auto dest = projectRoot / files::FromUtf8(Replace(module.get<std::string>(), "\\", "/") + ".csc");
            auto tokens = Tokenize(files::ReadText(dest));
            for (long long i = 0; i < static_cast<long long>(tokens.size()); i++)
                if (tokens[i].kind == Kind::String && StartsWith(tokens[i].text, "\"") && !LookupKey(tokens, i))
                    BindLiteral(tokens[i], effects, weapons, soundNames);
            files::WriteText(dest, Emit(tokens));
        }
        std::vector<std::string> events;
        if (report.contains("events"))
            for (const auto& e : report["events"])
                events.push_back(e.get<std::string>());
        const auto fxModule = std::string(PORTED_PREFIX) + "_fx";
        std::string forwards, handlers;
        for (size_t i = 0; i < events.size(); i++)
        {
            forwards += "    forward_notify( \"" + events[i] + "\", ::waw_notify_" + std::to_string(i) + " );\n";
            handlers += "\nwaw_notify_" + std::to_string(i) + "( localclientnum, state )\n{\n    waw_deliver( \"" + events[i] + "\", localclientnum );\n}\n";
        }
        auto glue = std::string(PORT_HEADER) + std::string(CLIENT_GLUE_SOURCE);
        glue = Replace(glue, "@FORWARDS@", forwards);
        glue = Replace(glue, "@ENTRY@", report["entry"].get<std::string>());
        glue = Replace(glue, "@CONNECT@", InList(modules, fxModule) ? "    " + fxModule + "::fx_init( localclientnum );\n" : "");
        glue = Replace(glue, "@HANDLERS@", handlers);
        files::WriteText(projectRoot / files::FromUtf8(Replace(CLIENT_GLUE, "\\", "/") + ".csc"), glue);
        return "    " + std::string(CLIENT_GLUE) + "::init();\n";
    }
} // namespace w2b::gsc::port::clientport
