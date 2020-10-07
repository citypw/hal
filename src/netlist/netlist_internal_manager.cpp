#include "hal_core/netlist/netlist_internal_manager.h"

#include "hal_core/netlist/endpoint.h"
#include "hal_core/netlist/event_system/gate_event_handler.h"
#include "hal_core/netlist/event_system/module_event_handler.h"
#include "hal_core/netlist/event_system/net_event_handler.h"
#include "hal_core/netlist/gate.h"
#include "hal_core/netlist/gate_library/gate_type/gate_type.h"
#include "hal_core/netlist/module.h"
#include "hal_core/netlist/net.h"
#include "hal_core/netlist/netlist.h"
#include "hal_core/utilities/log.h"

namespace hal
{
    NetlistInternalManager::NetlistInternalManager(Netlist* nl) : m_netlist(nl)
    {
        assert(nl != nullptr);
    }

    template<typename T>
    static void unordered_vector_erase(std::vector<T>& vec, T element)
    {
        auto it = std::find(vec.begin(), vec.end(), element);
        if (it == vec.end())
        {
            log_critical("netlist.internal", "element that is guaranteed to be there is not there!");
        }
        *it = vec.back();
        vec.pop_back();
    }

    //######################################################################
    //###                      gates                                     ###
    //######################################################################

    Gate* NetlistInternalManager::create_gate(const u32 id, const GateType* gt, const std::string& name, float x, float y)
    {
        if (id == 0)
        {
            log_error("netlist.internal", "netlist::create_gate: id 0 represents 'invalid ID'.");
            return nullptr;
        }
        if (m_netlist->m_used_gate_ids.find(id) != m_netlist->m_used_gate_ids.end())
        {
            log_error("netlist.internal", "netlist::create_gate: gate id {:08x} is already taken.", id);
            return nullptr;
        }
        if (gt == nullptr) {
            log_error("netlist.internal", "netlist::create_gate: nullptr given for gate type.", id);
            return nullptr;
        }
        if (this->is_gate_type_invalid(gt))
        {
            log_error("netlist.internal", "netlist::create_gate: gate type '{}' is invalid.", gt->get_name());
            return nullptr;
        }
        if (utils::trim(name).empty())
        {
            log_error("netlist.internal", "netlist::create_gate: empty name is not allowed.");
            return nullptr;
        }

        auto new_gate = std::unique_ptr<Gate>(new Gate(this, id, gt, name, x, y));

        auto free_id_it = m_netlist->m_free_gate_ids.find(id);
        if (free_id_it != m_netlist->m_free_gate_ids.end())
        {
            m_netlist->m_free_gate_ids.erase(free_id_it);
        }

        m_netlist->m_used_gate_ids.insert(id);

        // add gate to top module
        new_gate->m_module = m_netlist->m_top_module;

        auto raw = new_gate.get();

        m_netlist->m_gates_map[id] = std::move(new_gate);
        m_netlist->m_gates_set.insert(raw);
        m_netlist->m_gates.push_back(raw);

        m_netlist->m_top_module->m_gates_map[id] = raw;
        m_netlist->m_top_module->m_gates.push_back(raw);

        // notify
        module_event_handler::notify(module_event_handler::event::gate_assigned, m_netlist->m_top_module, id);
        gate_event_handler::notify(gate_event_handler::event::created, raw);

        return raw;
    }

    bool NetlistInternalManager::delete_gate(Gate* gate)
    {
        if (!m_netlist->is_gate_in_netlist(gate))
        {
            return false;
        }

        for (auto ep : gate->get_fan_out_endpoints())
        {
            if (!net_remove_source(ep->get_net(), ep))
            {
                return false;
            }
        }

        for (auto ep : gate->get_fan_in_endpoints())
        {
            if (!net_remove_destination(ep->get_net(), ep))
            {
                return false;
            }
        }

        // check global_gnd and global_vcc gates
        m_netlist->unmark_gnd_gate(gate);
        m_netlist->unmark_vcc_gate(gate);

        // remove gate from modules
        gate->m_module->m_gates_map.erase(gate->m_module->m_gates_map.find(gate->get_id()));
        unordered_vector_erase(gate->m_module->m_gates, gate);

        auto it  = m_netlist->m_gates_map.find(gate->get_id());
        auto ptr = std::move(it->second);
        m_netlist->m_gates_map.erase(it);
        m_netlist->m_gates_set.erase(gate);
        unordered_vector_erase(m_netlist->m_gates, gate);

        // free ids
        m_netlist->m_free_gate_ids.insert(gate->get_id());
        m_netlist->m_used_gate_ids.erase(gate->get_id());

        module_event_handler::notify(module_event_handler::event::gate_removed, gate->m_module, gate->get_id());
        gate_event_handler::notify(gate_event_handler::event::removed, gate);

        return true;
    }

    bool NetlistInternalManager::is_gate_type_invalid(const GateType* gt) const
    {
        return !m_netlist->m_gate_library->contains_gate_type(gt);
    }

    //######################################################################
    //###                       nets                                     ###
    //######################################################################

    Net* NetlistInternalManager::create_net(const u32 id, const std::string& name)
    {
        if (id == 0)
        {
            log_error("netlist.internal", "netlist::create_net: id 0 represents 'invalid ID'.");
            return nullptr;
        }
        if (m_netlist->m_used_net_ids.find(id) != m_netlist->m_used_net_ids.end())
        {
            log_error("netlist.internal", "netlist::create_net: net id {:08x} is already taken.", id);
            return nullptr;
        }
        if (utils::trim(name).empty())
        {
            log_error("netlist.internal", "netlist::create_net: empty name is not allowed");
            return nullptr;
        }

        auto new_net = std::unique_ptr<Net>(new Net(this, id, name));

        auto free_id_it = m_netlist->m_free_net_ids.find(id);
        if (free_id_it != m_netlist->m_free_net_ids.end())
        {
            m_netlist->m_free_net_ids.erase(free_id_it);
        }

        m_netlist->m_used_net_ids.insert(id);

        // add net to netlist
        auto raw                  = new_net.get();
        m_netlist->m_nets_map[id] = std::move(new_net);
        m_netlist->m_nets_set.insert(raw);
        m_netlist->m_nets.push_back(raw);

        // notify
        net_event_handler::notify(net_event_handler::event::created, raw);

        return raw;
    }

    bool NetlistInternalManager::delete_net(Net* net)
    {
        if (!m_netlist->is_net_in_netlist(net))
        {
            return false;
        }

        auto dsts = net->m_destinations_raw;
        for (auto dst : dsts)
        {
            if (!this->net_remove_destination(net, dst))
            {
                return false;
            }
        }

        auto srcs = net->m_sources_raw;
        for (auto src : srcs)
        {
            if (!this->net_remove_source(net, src))
            {
                return false;
            }
        }

        // check global_input and global_output gates
        m_netlist->unmark_global_input_net(net);
        m_netlist->unmark_global_output_net(net);

        // remove net from netlist
        auto it  = m_netlist->m_nets_map.find(net->get_id());
        auto ptr = std::move(it->second);
        m_netlist->m_nets_map.erase(it);
        m_netlist->m_nets_set.erase(net);
        unordered_vector_erase(m_netlist->m_nets, net);

        m_netlist->m_free_net_ids.insert(net->get_id());
        m_netlist->m_used_net_ids.erase(net->get_id());

        net_event_handler::notify(net_event_handler::event::removed, net);

        return true;
    }

    Endpoint* NetlistInternalManager::net_add_source(Net* net, Gate* gate, const std::string& pin)
    {
        if (!m_netlist->is_net_in_netlist(net) || !m_netlist->is_gate_in_netlist(gate))
        {
            return nullptr;
        }

        if (net->is_a_source(gate, pin))
        {
            log_error("netlist.internal", "net::add_source: src gate ('{}',  type = {}) is already added to net '{}'.", gate->get_name(), gate->get_type()->get_name(), net->get_name());
            return nullptr;
        }

        // check whether pin id is valid for this gate
        auto output_pins = gate->get_type()->get_output_pins();

        if ((std::find(output_pins.begin(), output_pins.end(), pin) == output_pins.end()))
        {
            log_error("netlist.internal", "net::add_source: src gate ('{}',  type = {}) has no output type '{}'.", gate->get_name(), gate->get_type()->get_name(), pin);
            return nullptr;
        }

        // check whether src has already an assigned net
        if (gate->get_fan_out_net(pin) != nullptr)
        {
            log_error("netlist.internal",
                      "net::add_source: gate '{}' already has an assigned net '{}' for output pin '{}', cannot assign new net '{}'.",
                      gate->get_name(),
                      gate->get_fan_out_net(pin)->get_name(),
                      pin,
                      net->get_name());
            return nullptr;
        }

        auto new_endpoint     = std::unique_ptr<Endpoint>(new Endpoint(gate, pin, net, false));
        auto new_endpoint_raw = new_endpoint.get();
        net->m_sources.push_back(std::move(new_endpoint));
        net->m_sources_raw.push_back(new_endpoint_raw);
        gate->m_out_endpoints.push_back(new_endpoint_raw);
        gate->m_out_nets.push_back(net);

        net_event_handler::notify(net_event_handler::event::src_added, net, gate->get_id());

        return new_endpoint_raw;
    }

    bool NetlistInternalManager::net_remove_source(Net* net, Endpoint* ep)
    {
        auto gate = ep->get_gate();

        if (!m_netlist->is_net_in_netlist(net) || !m_netlist->is_gate_in_netlist(gate) || !net->is_a_source(ep))
        {
            return false;
        }

        bool removed = false;
        for (u32 i = 0; i < net->m_sources.size(); ++i)
        {
            if (net->m_sources_raw[i] == ep)
            {
                unordered_vector_erase(gate->m_out_endpoints, ep);
                unordered_vector_erase(gate->m_out_nets, net);
                net->m_sources[i] = std::move(net->m_sources.back());
                net->m_sources.pop_back();
                net->m_sources_raw[i] = net->m_sources_raw.back();
                net->m_sources_raw.pop_back();
                net_event_handler::notify(net_event_handler::event::src_removed, net, gate->get_id());
                removed = true;
                break;
            }
        }

        if (!removed)
        {
            log_warning("nelist.internal", "net::remove_source: net '{}' has no src gate '{}' at pin '{}'", net->get_name(), gate->get_name(), ep->get_pin());
        }

        return true;
    }

    Endpoint* NetlistInternalManager::net_add_destination(Net* net, Gate* gate, const std::string& pin)
    {
        if (!m_netlist->is_net_in_netlist(net) || !m_netlist->is_gate_in_netlist(gate))
        {
            return nullptr;
        }

        if (net->is_a_destination(gate, pin))
        {
            log_error("netlist.internal", "net::add_destination: dst gate ('{}',  type = {}) is already added to net '{}'.", gate->get_name(), gate->get_type()->get_name(), net->get_name());
            return nullptr;
        }

        // check whether pin id is valid for this gate
        auto input_pins = gate->get_type()->get_input_pins();

        if ((std::find(input_pins.begin(), input_pins.end(), pin) == input_pins.end()))
        {
            log_error("netlist.internal", "net::add_destination: dst gate ('{}',  type = {}) has no input type '{}'.", gate->get_name(), gate->get_type()->get_name(), pin);
            return nullptr;
        }

        // check whether dst has already an assigned net
        if (gate->get_fan_in_net(pin) != nullptr)
        {
            log_error("netlist.internal",
                      "net::add_destination: gate '{}' already has an assigned net '{}' for input pin '{}', cannot assign new net '{}'.",
                      gate->get_name(),
                      gate->get_fan_in_net(pin)->get_name(),
                      pin,
                      net->get_name());
            return nullptr;
        }

        auto new_endpoint     = std::unique_ptr<Endpoint>(new Endpoint(gate, pin, net, true));
        auto new_endpoint_raw = new_endpoint.get();
        net->m_destinations.push_back(std::move(new_endpoint));
        net->m_destinations_raw.push_back(new_endpoint_raw);
        gate->m_in_endpoints.push_back(new_endpoint_raw);
        gate->m_in_nets.push_back(net);

        net_event_handler::notify(net_event_handler::event::dst_added, net, gate->get_id());

        return new_endpoint_raw;
    }

    bool NetlistInternalManager::net_remove_destination(Net* net, Endpoint* ep)
    {
        auto gate = ep->get_gate();
        if (!m_netlist->is_net_in_netlist(net) || !m_netlist->is_gate_in_netlist(gate) || !net->is_a_destination(ep))
        {
            return false;
        }

        bool removed = false;
        for (u32 i = 0; i < net->m_destinations.size(); ++i)
        {
            if (net->m_destinations_raw[i] == ep)
            {
                unordered_vector_erase(gate->m_in_endpoints, ep);
                unordered_vector_erase(gate->m_in_nets, net);
                net->m_destinations[i] = std::move(net->m_destinations.back());
                net->m_destinations.pop_back();
                net->m_destinations_raw[i] = net->m_destinations_raw.back();
                net->m_destinations_raw.pop_back();
                net_event_handler::notify(net_event_handler::event::dst_removed, net, gate->get_id());
                removed = true;
                break;
            }
        }

        if (!removed)
        {
            log_warning("nelist.internal", "net::remove_source: net '{}' has no src gate '{}' at pin '{}'", net->get_name(), gate->get_name(), ep->get_pin());
        }

        return true;
    }

    //######################################################################
    //###                          modules                               ###
    //######################################################################

    Module* NetlistInternalManager::create_module(const u32 id, Module* parent, const std::string& name)
    {
        if (id == 0)
        {
            log_error("netlist.internal", "netlist::create_module: id 0 represents 'invalid ID'.");
            return nullptr;
        }
        if (m_netlist->m_used_module_ids.find(id) != m_netlist->m_used_module_ids.end())
        {
            log_error("netlist.internal", "netlist::create_module: module id {:08x} is already taken.", id);
            return nullptr;
        }
        if (utils::trim(name).empty())
        {
            log_error("netlist.internal", "netlist::create_module: empty name is not allowed");
            return nullptr;
        }
        if (parent == nullptr && m_netlist->m_top_module != nullptr)
        {
            log_error("netlist.internal", "netlist::create_module: parent must not be nullptr");
            return nullptr;
        }
        if (parent != nullptr && m_netlist != parent->get_netlist())
        {
            log_error("netlist.internal", "netlist::create_module: parent must belong to current netlist");
            return nullptr;
        }

        auto m = std::unique_ptr<Module>(new Module(id, parent, name, this));

        auto free_id_it = m_netlist->m_free_module_ids.find(id);
        if (free_id_it != m_netlist->m_free_module_ids.end())
        {
            m_netlist->m_free_module_ids.erase(free_id_it);
        }

        m_netlist->m_used_module_ids.insert(id);

        auto raw                     = m.get();
        m_netlist->m_modules_map[id] = std::move(m);
        m_netlist->m_modules_set.insert(raw);
        m_netlist->m_modules.push_back(raw);

        if (parent != nullptr)
        {
            parent->m_submodules_map[id] = raw;
            parent->m_submodules.push_back(raw);
        }

        module_event_handler::notify(module_event_handler::event::created, raw);

        if (parent != nullptr)
        {
            module_event_handler::notify(module_event_handler::event::submodule_added, parent, id);
        }

        return raw;
    }

    bool NetlistInternalManager::delete_module(Module* to_remove)
    {
        if (!m_netlist->is_module_in_netlist(to_remove))
        {
            return false;
        }

        if (to_remove == m_netlist->m_top_module)
        {
            return false;
        }

        // at this point parent is guaranteed to be not null

        // move gates and nets to parent, work on a copy since assign_gate will modify m_gates
        auto gates_copy = to_remove->m_gates;
        for (auto gate : gates_copy)
        {
            to_remove->m_parent->assign_gate(gate);
        }

        // move all submodules to parent
        for (auto sm : to_remove->m_submodules)
        {
            to_remove->m_parent->m_submodules_map[sm->get_id()] = sm;
            to_remove->m_parent->m_submodules.push_back(sm);

            module_event_handler::notify(module_event_handler::event::submodule_removed, sm->get_parent_module(), sm->get_id());

            sm->m_parent = to_remove->m_parent;

            module_event_handler::notify(module_event_handler::event::parent_changed, sm, 0);
            module_event_handler::notify(module_event_handler::event::submodule_added, to_remove->m_parent, sm->get_id());
        }

        // remove module from parent
        to_remove->m_parent->m_submodules_map.erase(to_remove->get_id());
        unordered_vector_erase(to_remove->m_parent->m_submodules, to_remove);
        module_event_handler::notify(module_event_handler::event::submodule_removed, to_remove->m_parent, to_remove->get_id());

        auto it  = m_netlist->m_modules_map.find(to_remove->get_id());
        auto ptr = std::move(it->second);
        m_netlist->m_modules_map.erase(it);
        m_netlist->m_modules_set.erase(to_remove);
        unordered_vector_erase(m_netlist->m_modules, to_remove);

        m_netlist->m_free_module_ids.insert(to_remove->get_id());
        m_netlist->m_used_module_ids.erase(to_remove->get_id());

        module_event_handler::notify(module_event_handler::event::removed, to_remove);
        return true;
    }

    bool NetlistInternalManager::module_assign_gate(Module* m, Gate* g)
    {
        if (g == nullptr)
        {
            return false;
        }
        if (g->m_module == m)
        {
            return false;
        }
        auto prev_module = g->m_module;

        prev_module->m_gates_map.erase(prev_module->m_gates_map.find(g->get_id()));
        unordered_vector_erase(prev_module->m_gates, g);

        m->m_gates_map[g->get_id()] = g;
        m->m_gates.push_back(g);

        g->m_module = m;

        module_event_handler::notify(module_event_handler::event::gate_removed, prev_module, g->get_id());
        module_event_handler::notify(module_event_handler::event::gate_assigned, m, g->get_id());
        return true;
    }

    bool NetlistInternalManager::module_remove_gate(Module* m, Gate* g)
    {
        if (g == nullptr)
        {
            return false;
        }

        if (m == m_netlist->m_top_module)
        {
            log_error("module", "cannot remove gates from top module.", g->get_name(), g->get_id(), m->get_name(), m->get_id());
            return false;
        }

        auto it = m->m_gates_map.find(g->get_id());

        if (it == m->m_gates_map.end())
        {
            log_error("module", "gate '{}' (id {}) is not stored in module '{}' (id {}).", g->get_name(), g->get_id(), m->get_name(), m->get_id());
            return false;
        }

        m->m_gates_map.erase(it);
        unordered_vector_erase(m->m_gates, g);

        m_netlist->m_top_module->m_gates_map[g->get_id()] = g;
        m_netlist->m_top_module->m_gates.push_back(g);
        g->m_module = m_netlist->m_top_module;

        module_event_handler::notify(module_event_handler::event::gate_removed, m, g->get_id());
        module_event_handler::notify(module_event_handler::event::gate_assigned, m_netlist->m_top_module, g->get_id());

        return true;
    }

    //######################################################################
    //###                           caches                               ###
    //######################################################################

    void NetlistInternalManager::clear_caches()
    {
        m_lut_function_cache.clear();
    }
}    // namespace hal
