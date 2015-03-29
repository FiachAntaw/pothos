// Copyright (c) 2014-2015 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "EvalEnvironment.hpp"
#include <Pothos/Util/Compiler.hpp>
#include <Pothos/Util/EvalInterface.hpp>
#include <Pothos/Object.hpp>
#include <Pothos/Object/Containers.hpp>
#include <Pothos/Proxy.hpp>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <mutex>
#include "mpParser.h"

static const std::string mapTypeId("__map__B098D7A2__");

/***********************************************************************
 * convert parser value into a native object
 **********************************************************************/
static Pothos::Object mupValueToObject(mup::IValue &val)
{
    switch (val.GetType())
    {
    case 'b': return Pothos::Object(val.GetBool());
    case 'i': return Pothos::Object(val.GetInteger());
    case 'f': return Pothos::Object(val.GetFloat());
    case 'c': return Pothos::Object(val.GetComplex());
    case 's': return Pothos::Object(val.GetString());
    case 'm': break;
    default: Pothos::Exception("EvalEnvironment::mupValueToObject()", "unknown type " + val.AsciiDump());
    }

    assert(val.GetType() == 'm');
    auto env = Pothos::ProxyEnvironment::make("managed");

    //detect if this array is a flattened map
    const bool isMap = (val.GetCols() % 2) == 1 and
        val.At(0, 0).GetType() == 's' and
        val.At(0, 0).GetString() == mapTypeId;

    //support array to vector
    Pothos::ProxyVector vec(val.GetCols());
    for (size_t i = 0; i < vec.size(); i++)
    {
        const auto obj_i = mupValueToObject(val.At(0, i));
        vec[i] = env->convertObjectToProxy(obj_i);
    }
    if (not isMap) return Pothos::Object(vec);

    //special case map mode (array -> vector -> map)
    Pothos::ProxyMap map;
    for (size_t i = 0; i < vec.size()/2; i++)
    {
        map[vec[i*2 + 1]] = vec[i*2 + 2];
    }
    return Pothos::Object(map);
}

/***********************************************************************
 * convert native object into a parser value
 **********************************************************************/
static mup::Value objectToMupValue(const Pothos::Object &obj)
{
    if (obj.type() == typeid(mup::string_type)) return mup::Value(obj.extract<mup::string_type>());
    if (obj.type() == typeid(mup::float_type)) return mup::Value(obj.extract<mup::float_type>());
    if (obj.type() == typeid(mup::bool_type)) return mup::Value(obj.extract<mup::bool_type>());
    if (obj.type() == typeid(mup::int_type)) return mup::Value(obj.extract<mup::int_type>());
    if (obj.type() == typeid(mup::cmplx_type)) return mup::Value(obj.extract<mup::cmplx_type>());

    //support proxy vector to parser array
    if (obj.type() == typeid(Pothos::ProxyVector))
    {
        const auto &vec = obj.extract<Pothos::ProxyVector>();
        mup::Value arr(1, vec.size(), 0.0);
        for (size_t i = 0; i < vec.size(); i++)
        {
            const auto obj_i = vec[i].getEnvironment()->convertProxyToObject(vec[i]);
            arr.At(0, i) = objectToMupValue(obj_i);
        }
        return arr;
    }

    //support proxy map to parser array
    if (obj.type() == typeid(Pothos::ProxyMap))
    {
        const auto &map = obj.extract<Pothos::ProxyMap>();
        mup::Value arr(1, map.size()*2+1, 0.0);
        size_t i = 0;
        arr.At(0, i++) = mup::Value(mapTypeId);
        for (const auto &pair : map)
        {
            const auto key_i = pair.first.getEnvironment()->convertProxyToObject(pair.first);
            const auto val_i = pair.second.getEnvironment()->convertProxyToObject(pair.second);
            arr.At(0, i++) = objectToMupValue(key_i);
            arr.At(0, i++) = objectToMupValue(val_i);
        }
        return arr;
    }

    throw Pothos::Exception("EvalEnvironment::objectToMupValue()", "unknown type " + obj.getTypeString());
}

/***********************************************************************
 * Evaluator implementation
 **********************************************************************/
struct EvalEnvironment::Impl
{
    Impl(void):
        p(mup::pckALL_COMPLEX)
    {
        p.DefineConst("True", true);
        p.DefineConst("False", false);
        p.DefineConst("j", std::complex<double>(0.0, 1.0));
    }
    std::mutex parserMutex;
    mup::ParserX p;
};

EvalEnvironment::EvalEnvironment(void):
    _impl(new Impl())
{
    return;
}

void EvalEnvironment::registerConstantExpr(const std::string &key, const std::string &expr)
{
    try
    {
        const auto result = objectToMupValue(this->eval(expr));
        if (_impl->p.IsConstDefined(key)) _impl->p.RemoveConst(key);
        _impl->p.DefineConst(key, result);
    }
    catch (const mup::ParserError &ex)
    {
        throw Pothos::Exception("EvalEnvironment::eval("+expr+")", ex.GetMsg());
    }
}

void EvalEnvironment::registerConstantObj(const std::string &key, const Pothos::Object &obj)
{
    const auto result = objectToMupValue(obj);
    if (_impl->p.IsConstDefined(key)) _impl->p.RemoveConst(key);
    _impl->p.DefineConst(key, result);
}

Pothos::Object EvalEnvironment::eval(const std::string &expr)
{
    if (expr.empty()) throw Pothos::Exception("EvalEnvironment::eval()", "expression is empty");
    const auto inBrackets = expr.size() >= 2 and expr.front() == '[' and expr.back() == ']';
    const auto inBraces = expr.size() >= 2 and expr.front() == '{' and expr.back() == '}';

    //list syntax mode
    if (inBrackets)
    {
        auto env = Pothos::ProxyEnvironment::make("managed");
        Pothos::ProxyVector vec;
        const auto noBrackets = expr.substr(1, expr.size()-2);
        for (const auto &tok : EvalEnvironment::splitExpr(noBrackets, ','))
        {
            try
            {
                vec.emplace_back(env->convertObjectToProxy(this->eval(tok)));
            }
            catch (const Pothos::Exception &ex)
            {
                throw Pothos::Exception("EvalEnvironment::eval("+expr+")", ex.message());
            }
        }
        return Pothos::Object(vec);
    }

    //map syntax mode
    if (inBraces)
    {
        auto env = Pothos::ProxyEnvironment::make("managed");
        Pothos::ProxyMap map;
        const auto noBrackets = expr.substr(1, expr.size()-2);
        for (const auto &tok : EvalEnvironment::splitExpr(noBrackets, ','))
        {
            const auto keyVal = EvalEnvironment::splitExpr(tok, ':');
            if (keyVal.size() != 2) throw Pothos::Exception("EvalEnvironment::eval("+tok+")", "not key:value");
            try
            {
                const auto key = env->convertObjectToProxy(this->eval(keyVal[0]));
                const auto val = env->convertObjectToProxy(this->eval(keyVal[1]));
                map.emplace(key, val);
            }
            catch (const Pothos::Exception &ex)
            {
                throw Pothos::Exception("EvalEnvironment::eval("+expr+")", ex.message());
            }
        }
        return Pothos::Object(map);
    }

    try
    {
        std::lock_guard<std::mutex> lock(_impl->parserMutex);
        _impl->p.SetExpr(expr);
        mup::Value result = _impl->p.Eval();
        return mupValueToObject(result);
    }
    catch (const mup::ParserError &ex)
    {
        throw Pothos::Exception("EvalEnvironment::eval("+expr+")", ex.GetMsg());
    }

    throw Pothos::Exception("EvalEnvironment::eval("+expr+")", "unknown result");
}

#include <Pothos/Managed.hpp>

static auto managedEvalEnvironment = Pothos::ManagedClass()
    .registerConstructor<EvalEnvironment>()
    .registerStaticMethod(POTHOS_FCN_TUPLE(EvalEnvironment, make))
    .registerMethod(POTHOS_FCN_TUPLE(EvalEnvironment, eval))
    .registerMethod(POTHOS_FCN_TUPLE(EvalEnvironment, registerConstantExpr))
    .registerMethod(POTHOS_FCN_TUPLE(EvalEnvironment, registerConstantObj))
    .commit("Pothos/Util/EvalEnvironment");
