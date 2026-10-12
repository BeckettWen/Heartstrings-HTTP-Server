// this is the state machine tool
// Notice that although these tools are separated in different files
// the version is all the same

#pragma once

#include <expected>
#include <type_traits>
#include <functional>
#include <any>
#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <unordered_map>
#include <memory>
#include <algorithm>

#include "VersionInfo.h"

using event_type_General = std::function<void()>;

namespace Aegis_stateMachine{


    template<typename State, typename Events>
    class StateMachine{
    private:
        // stores the current state
        std::vector<State> state_set;
        State current_state;

        using transition_rule = std::tuple<Events, State, event_type_General, State>;
        std::unordered_map<State, transition_rule> registry;

    public:

        explicit StateMachine(const State& state): current_state(state){};
        ~StateMachine(){};

        std::expected<void, std::string> Store_State(State& current_state_input) {
            current_state = current_state_input;
            return {};
        }

        void Register_Rules_With_Events(State& state, event_type_General action, Events event, State& next_state) {
            // check if the state exists, if not, must be a new state
            typename std::vector<State>::iterator it = std::find(state_set.begin(), state_set.end(), state);
            if (it == state_set.end()){ state_set.emplace_back(state); }

            // register the rules, which is still the prototype design
            std::shared_ptr<transition_rule> rule = std::make_shared<transition_rule>(event, state, action, next_state);
            registry.insert({state, (*rule)});
        }

        std::expected<void, std::string> Transition(State& state) {
            // add the check to ensure the safety
            if(state != current_state){
                return std::unexpected<std::string>("State Rule Not Found");
            }
            
            // here first find the tuple that stores the transition rule
            typename std::unordered_map<State, transition_rule>::iterator find_result = registry.find(state);
            if (find_result == registry.end()) {
                return std::unexpected<std::string>("State Rule Not Found");
            }

            // execute the action or event?
            // get it, execute the action
            event_type_General ongoing_action = std::get<2>(find_result->second);
            if (ongoing_action){ongoing_action();}

            current_state = std::get<3>(find_result->second);

            return {};
        }
    };
}
