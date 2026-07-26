#pragma once

#include "order.hpp"

namespace me {

class IOrderQueue {
public:
    virtual void push(Order o)    = 0;
    virtual bool pop(Order& out)  = 0;   // blocking with shutdown escape
    virtual void shutdown()       = 0;   // signal consumer to stop after draining
    virtual ~IOrderQueue()        = default;
};

} // namespace me
