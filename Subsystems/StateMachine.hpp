// this is the state machine tool

#pragma once

#include <expected>
#include <type_traits>
#include <functional>
#include <any>

using event_type_General = std::function<void()>;

namespace Aegis_stateMachine{

    // here is the error code that help define the error
    enum Error_Code: uint{
        Requested_State_Event_Not_Found = 0,
        Boundary = 1024
    };

    enum Switching_Status: uint{
        Success = 0,
        Against_Boundary = 1
    };

    std::any stored_State{};

    // this is the general API you use to set the initial state
    template<typename type>
        requires std::is_enum_v<type>
    std::expected<void, std::string> InitializeState(type& initial_state){
        try{
            stored_State = std::ref(initial_state);
        }
        catch(const std::exception& e){
            return std::unexpected<std::string>(e.what());
        }
        
        return {};
    }

    // retrieve the data inside the 'Any type enumeration' 
    template<typename type>
    type& retrieve_value(){
        return std::any_cast<std::reference_wrapper<type>>(stored_State).get();
    }

    template<typename type>
    class StateMachine{
        // stores the current state
        std::any current_state;

        std::unordered_map<std::any, event_type_General> event_Registration;
        std::unordered_map<event_type_General, std::any> reverse_event_Registration;

        StateMachine<type>(const StateMachine<type>&) = delete;
        ~StateMachine<type>(){};

        void switch_State(type& state) {
            current_state = state;
        }

        void Register_Event(type& state, event_type_General event) {
            // register the specific event to the state
            event_Registration.insert(event_Registration.end(), {state, event});

            // reversal registration of the event and the state
            // used for the later retrieve
            reverse_event_Registration.insert(reverse_event_Registration.end(), {event, state});
        }

        std::expected<Switching_Status, Error_Code> Transition(type& state) {
            std::unordered_map<std::any, event_type_General>::iterator find_result =
                event_Registration.find(state);

            // if there is no error, then fireup the event
            if (find_result == event_Registration.end()) {
                return std::unexpected<Error_Code>(Error_Code::Requested_State_Event_Not_Found);
            }

            // remove the redundant else keyword
            find_result->second();

            // move the state to the next state and terminate the function execuion
            if (state != Boundary) {
                state++;
                return Switching_Status::Success;
            }

            // make sure the return value exists in every circumstance
            return Switching_Status::Success;
        }
    };
}