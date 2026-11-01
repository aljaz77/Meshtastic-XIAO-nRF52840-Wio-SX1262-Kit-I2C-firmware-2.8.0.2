#pragma once
// Same shape as Meshtastic's src/Observer.h (CallbackObserver / Observable), simplified.
#include <list>

template <class T> class Observable;

template <class T> class Observer
{
  public:
    virtual ~Observer() = default;
    void observe(Observable<T> *o) { o->observers.push_back(this); }
    virtual int onNotify(T arg) = 0;
};

template <class Callback, class T> class CallbackObserver : public Observer<T>
{
    typedef int (Callback::*ObserverCallback)(T arg);
    Callback *objPtr;
    ObserverCallback method;

  public:
    CallbackObserver(Callback *_objPtr, ObserverCallback _method) : objPtr(_objPtr), method(_method) {}
    int onNotify(T arg) override { return (objPtr->*method)(arg); }
};

template <class T> class Observable
{
  public:
    std::list<Observer<T> *> observers;
    void notifyObservers(T arg)
    {
        for (auto *o : observers)
            o->onNotify(arg);
    }
};
