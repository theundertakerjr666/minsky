/*
  @copyright Steve Keen 2012
  @author Russell Standish
  This file is part of Minsky.

  Minsky is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Minsky is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Minsky.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "equations.h"
#include "expr.h"
#include "minsky.h"
#include "str.h"
#include "flowCoef.h"
#include <ecolab_epilogue.h>
using namespace minsky;

namespace MathDAG
{

  namespace
  {
    // comparison function for sorting variables into their definition order
    struct VariableDefOrder
    {
      unsigned maxOrder;
      VariableDefOrder(unsigned maxOrder): maxOrder(maxOrder) {}
      bool operator()(const VariableDAG* x, const VariableDAG* y) {
        return x->order(maxOrder)<y->order(maxOrder);
      }
    };

    struct NoArgument: public std::exception
    {
      OperationPtr state;
      unsigned argNum1, argNum2;
      NoArgument(const OperationPtr& s, unsigned a1, unsigned a2): 
        state(s), argNum1(a1), argNum2(a2) {}
      const char* what() const noexcept override {
        string r="missing argument "+to_string(argNum1)+","+to_string(argNum2)+
          " on operation ";
        if (state)
          {
            minsky::minsky().displayErrorItem(*state);
            r+=OperationType::typeName(state->type());
          }
        
        return r.c_str();
      }
    };

  }

  VariableValue ConstantDAG::addEvalOps
  (EvalOpVector& ev, VariableValue* r)
  {
    if (!result || result->idx()<0)
      {
        VariableValues& values=minsky::minsky().variableValues;
        if (r && r->isFlowVar())
          {
            result=r;
            // set the initial value of the actual variableValue (if it exists)
            auto v=values.find(result->name);
            if (v!=values.end())
              v->second.init=value;
          }
        else
          result=&tmpResult;
        result->init=value;
        *result=result->initValue(values);
      }
    if (r && r->isFlowVar() && r!=result)
      ev.push_back(EvalOpPtr(OperationType::copy, nullptr, *r, *result));
    assert(result->idx()>=0);
    doOneEvent(true);
    return *result;
  }

  VariableValue VariableDAG::addEvalOps
  (EvalOpVector& ev, VariableValue* r)
  {
    if (!result || result->idx()<0)
      {
        assert(VariableValue::isValueId(valueId));
        auto ri=minsky::minsky().variableValues.find(valueId);
        if (ri==minsky::minsky().variableValues.end())
          ri=minsky::minsky().variableValues.emplace(valueId,VariableValue(VariableType::tempFlow)).first;
        result=&ri->second;
        if (result->idx()<0) result->allocValue();
        if (rhs)
          rhs->addEvalOps(ev, result);
      }
    if (r && r->isFlowVar() && (r!=result || !result->isFlowVar()))
      ev.push_back(EvalOpPtr(OperationType::copy, nullptr, *r, *result));
    assert(result->idx()>=0);
    doOneEvent(true);
    return *result;
  }

  VariableValue IntegralInputVariableDAG::addEvalOps
  (EvalOpVector& ev, VariableValue* r)
  {
    if (!result)
      {
        if (r && r->isFlowVar())
          result=r;
        else
          {
            result=&tmpResult;
            //            result.allocValue();
          }
        if (rhs)
          rhs->addEvalOps(ev, result);
        else 
          throw runtime_error("integral not defined for "+name);
      }
    if (r && r->isFlowVar() && (r!=result || !result->isFlowVar()))
      ev.push_back(EvalOpPtr(OperationType::copy, nullptr, *r, *result));
    assert(result->idx()>=0);
    doOneEvent(true);
    return *result;
  }

  namespace {OperationFactory<OperationDAGBase, OperationDAG, 
                              OperationType::numOps-1> operationDAGFactory;}

  OperationDAGBase* OperationDAGBase::create(Type type, const string& name)
  {
    auto r=operationDAGFactory.create(type);
    r->name=name;
    return r;
  }

  // the definition order of an operation is simply the maximum of all
  // of its arguments
  int OperationDAGBase::order(unsigned maxOrder) const
  {
    if (type()==integrate)
      return 0; //integrals have already been evaluated
    if (cachedOrder>=0) return cachedOrder;

    if (maxOrder==0)
      throw error("maximum order recursion reached");

    
    // constants have order one, as they must be ordered after the
    // "fake" variables have been initialised
    int order=type()==constant? 1: 0;
    for (size_t i=0; i<arguments.size(); ++i)
      for (size_t j=0; j<arguments[i].size(); ++j)
        {
          checkArg(i,j);
          order=std::max(order, arguments[i][j]->order(maxOrder-1));
        }
    return cachedOrder=order;
  }

  void OperationDAGBase::checkArg(unsigned i, unsigned j) const
  {
    if (arguments.size()<=i || arguments[i].size()<=j || !arguments[i][j])
      throw NoArgument(state, i, j);
  }


  namespace
  {
    void cumulate(EvalOpVector& ev, const std::shared_ptr<OperationBase>& state, VariableValue& r,
                  const vector<vector<VariableValue> >& argIdx,
                  OperationType::Type op, OperationType::Type accum, double groupIdentity)
    {
      // check if any arguments have x-vectors, and if so, initialise r.xVector
      size_t oldNumElems=r.numElements();
      for (auto& i: argIdx)
        if (i.size() && !i[0].xVector.empty())
          {
            // initialise r's xVector
            r.setXVector(i[0].xVector);
            break;
          }
      if (!r.xVector.empty())
        {
          // ensure dimensions are correct
          for (auto& i: argIdx)
            for (auto& j: i)
              r.makeXConformant(j);
          if (r.xVector.empty())
            return; // no common intersection amongst arguments
        }
      if (r.idx()==-1) r.allocValue();

      // short circuit multiplications if any of the terms are constant zero
      if (accum==OperationType::multiply)
        {
          if (op==OperationType::divide)
            for (auto& i: argIdx[1])
              if (i.isZero())
                throw runtime_error("divide by constant zero");
          for (auto& i: argIdx)
            for (auto& j: i)
              if (j.isZero())
                {
                  r=j;
                  return;
                }
        }
      
      if (argIdx.size()>0 && !argIdx[0].empty())
        {
          size_t i=0;
          if (accum==OperationType::add)
            while (i<argIdx[0].size() && argIdx[0][i].isZero())
              i++;
          if (i<argIdx[0].size())
            {
              ev.push_back(EvalOpPtr(OperationType::copy, state, r, argIdx[0][i]));
              for (++i; i<argIdx[0].size(); ++i)
                if (accum!=OperationType::add || !argIdx[0][i].isZero())
                  ev.push_back(EvalOpPtr(accum, state, r, r, argIdx[0][i]));
            }
        }
      else
        {
          //TODO: could be cleaned up if we don't need to support constant operators
          ev.push_back(EvalOpPtr(OperationType::constant, state, r));
          dynamic_cast<ConstantEvalOp&>(*ev.back()).value=groupIdentity;
        }

      if (argIdx.size()>1)
        {
          if (argIdx[1].size()==1)
            // eliminate redundant copy operation when only one wire
            ev.push_back(EvalOpPtr(op, state, r, r, argIdx[1][0]));
          else if (argIdx[1].size()>1)
            {
              // multiple wires to second input port
              VariableValue tmp(VariableType::tempFlow);
              tmp.setXVector(r.xVector);
              tmp.dims(r.dims());
              size_t i=0;
              if (accum==OperationType::add)
                while (i<argIdx[1].size() && argIdx[1][i].isZero()) i++;
              if (i<argIdx[1].size())
                {
                  ev.push_back(EvalOpPtr(OperationType::copy, state, tmp, argIdx[1][i]));
                  for (++i; i<argIdx[1].size(); ++i)
                    if (accum!=OperationType::add || !argIdx[1][i].isZero())
                      ev.push_back(EvalOpPtr(accum, state, tmp, tmp, argIdx[1][i]));
                  ev.push_back(EvalOpPtr(op, state, r, r, tmp));
                }
            }
        }
    }
  }

  VariableValue OperationDAGBase::addEvalOps
  (EvalOpVector& ev, VariableValue* r)
  {
    if (!result)
      {
        assert(!dynamic_cast<IntOp*>(state.get()));
        if (r && r->isFlowVar())
          result=r;
        else
          result=&tmpResult;

        // prepare argument expressions
        vector<vector<VariableValue> > argIdx(arguments.size());
        for (size_t i=0; type()!=integrate && i<arguments.size(); ++i)
          for (size_t j=0; j<arguments[i].size(); ++j)
            if (arguments[i][j])
              argIdx[i].push_back(arguments[i][j]->addEvalOps(ev));
            else
              {
                argIdx[i].push_back(VariableValue(VariableValue::tempFlow));
                argIdx[i].back().allocValue();
              }

        try
          {
            // basic arithmetic is handled in a cumulative fashion
            switch (type())
              {
              case add:
                cumulate(ev, state, *result, argIdx, add, add, 0);
                break;
              case subtract:
                cumulate(ev, state, *result, argIdx, subtract, add, 0);
                break;
              case multiply:
                cumulate(ev, state, *result, argIdx, multiply, multiply, 1);
                break;
              case divide:
                cumulate(ev, state, *result, argIdx, divide, multiply, 1);
                break;
              case min:
                cumulate(ev, state, *result, argIdx, min, min, numeric_limits<double>::max());
                break;
              case max:
                cumulate(ev, state, *result, argIdx, max, max, -numeric_limits<double>::max());
                break;
              case and_:
                cumulate(ev, state, *result, argIdx, and_, and_, 1);
                break;
              case or_:
                cumulate(ev, state, *result, argIdx, or_, or_, 0);
                break;
              case constant:
                if (state) minsky::minsky().displayErrorItem(*state);
                throw error("Constant deprecated");
              case lt: case le: case eq:
                for (size_t i=0; i<arguments.size(); ++i)
                  if (arguments[i].empty())
                    {
                      argIdx[i].push_back(VariableValue(VariableValue::tempFlow));
                      argIdx[i].back().allocValue();
                      // ensure units are compatible (as we're doing comparisons with zero)
                      if (i>0)
                        argIdx[i][0].units=argIdx[i-1][0].units;
                      else if (arguments.size()>1 && !argIdx[1].empty())
                        argIdx[0][0].units=argIdx[1][0].units;
                    }
                ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0], argIdx[1][0])); 
                break;
              case runningSum: case runningProduct:
                {
                  if (argIdx.empty() || argIdx[0].empty())
                    throw error("input not wired");
                  result->setXVector(argIdx[0][0].xVector);
                  ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0]));
                  assert(state);
                  ev.back()->setTensorParams(argIdx[0][0],*state);
                }
                break;
              case difference:
                {
                  // implement the difference operator as a
                  // subtraction, by fiddling with the offsets of the
                  // second variableValue
                  EvalOpPtr op(subtract, state, *result, argIdx[0][0], argIdx[0][0]);
                  ev.push_back(op);
                  size_t stride, dimSz;
                  argIdx[0][0].computeStrideAndSize(state->axis, stride,dimSz);

                  // trim off leading or trailing components
                  auto xVector=result->xVector;
                  for (auto& i: xVector)
                    if (state->axis.empty() || i.name==state->axis)
                      {
                        if (state->arg>=i.size())
                          throw error("difference argument %g greater than vector length %ul",state->arg,(long)i.size());
                        if (state->arg>0)
                          i.erase(i.begin(), i.begin()+state->arg);
                        else
                          i.erase(i.end()+state->arg, i.end());
                        break;
                      }
                  result->setXVector(xVector);
                  
                  decltype(op->in1) in1;
                  decltype(op->in2) in2;
                  for (auto& i: op->in2)
                    {
                      assert(state);
                      assert(i.size()==1);
                      assert(i[0].weight==1);
                      auto j=((i[0].idx-argIdx[0][0].idx())/stride)%dimSz;
                      if (j>=state->arg && j<dimSz+state->arg)
                        {
                          // only transfer in bound references
                          in1.push_back(op->in1[&i-&op->in2[0]]);
                          i[0].idx-=stride*state->arg;
                          in2.push_back(i);
                        }
                    }
                  op->in1.swap(in1);
                  op->in2.swap(in2);
                  break;
                }
              case index: 
                {
                  auto xVector=argIdx[0][0].xVector;
                  for (auto& i: xVector)
                    if (state->axis.empty() || i.name==state->axis || xVector.size()==1)
                      {
                        i.dimension.type=Dimension::value;
                        i.dimension.units.clear();
                        for (size_t j=0; j<i.size(); ++j)
                          i[j]=double(j);
                        break;
                      }
                  result->setXVector(xVector);
                  ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0]));
                }                
                break;
              case gather:
                {
                  auto xVector=argIdx[0][0].xVector;
                  for (auto& i: xVector)
                    if (state->axis.empty() || i.name==state->axis || xVector.size()==1)
                      {
                        i.dimension.type=Dimension::value;
                        i.dimension.units.clear();
                        for (size_t j=0; j<i.size(); ++j)
                          i[j]=double(j);
                        break;
                      }
                  result->setXVector(xVector);
                  ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0], argIdx[1][0]));
                }                
                break;
              case data:
                if (argIdx.size()>0 && argIdx[0].size()==1)
                  ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0])); 
                else
                  throw error("inputs for highlighted operations incorrectly wired");
                break;
              case ravel:
                if (auto r=dynamic_cast<Ravel*>(state.get()))
                  {
                    if (argIdx.size()>0 && argIdx[0].size()==1)
                      {
                        // process ravel to dimension the result
                        // variable correctly (data may be bogus at
                        // this time)
                        r->loadDataCubeFromVariable(argIdx[0][0]);
                        r->loadDataFromSlice(*result);
                        ev.emplace_back(new RavelEvalOp(argIdx[0][0], *result));
                        ev.back()->state=state;
                      }
                    else
                      r->loadDataFromSlice(*result);
                  }
                break;
              default:
                // sanity check that the correct number of arguments is provided 
                bool correctlyWired=true;
                for (size_t i=0; i<arguments.size(); ++i)
                  correctlyWired &= arguments[i].size()==1;
                if (!correctlyWired)
                  throw error("inputs for highlighted operations incorrectly wired");
            
                switch (arguments.size())
                  {
                  case 0:
                    ev.push_back(EvalOpPtr(type(), state, *result));
                    break;
                  case 1:
                    ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0]));
                    break;
                  case 2:
                    ev.push_back(EvalOpPtr(type(), state, *result, argIdx[0][0], argIdx[1][0]));
                    break;
                  default:
                    throw error("Too many arguments");
                  }
              }
          }
        catch (const std::exception&)
          {
            if (state) minsky::minsky().displayErrorItem(*state);
            throw;
          }
      }
    if (type()!=integrate && r && r->isFlowVar() && result!=r)
      ev.push_back(EvalOpPtr(copy, state, *r, *result));
    if (state && !state->ports.empty() && state->ports[0]) 
      state->ports[0]->setVariableValue(*result);
    assert(result->idx()>=0);
    doOneEvent(true);
    return *result;
  }

  SystemOfEquations::SystemOfEquations(const Minsky& m): minsky(m)
  {
    expressionCache.insertAnonymous(zero);
    expressionCache.insertAnonymous(one);
    zero->result=&const_cast<Minsky&>(m).variableValues.find("constant:zero")->second;
    one->result=&const_cast<Minsky&>(m).variableValues.find("constant:one")->second;

    // store stock & integral variables for later reordering
    map<string, VariableDAG*> integVarMap;

    vector<pair<VariableDAGPtr,Wire*>> integralInputs;
    
    // search through operations looking for integrals
    minsky.model->recursiveDo
      (&Group::items,
       [&](const Items&, Items::const_iterator it){
        if (IntOp* i=dynamic_cast<IntOp*>(it->get()))
          {
            if (VariablePtr iv=i->intVar)
              {
                // .get() OK here because object lifetime controlled by
                // expressionCache
                VariableDAG* v=integVarMap[iv->valueId()]=
                  dynamic_cast<VariableDAG*>(makeDAG(*iv).get());
                v->intOp=i;
                if (i->ports[1]->wires().size()>0)
                  {
                    // with integrals, we need to create a distinct variable to
                    // prevent infinite recursion of order() in the case of graph cycles
                    VariableDAGPtr input(new IntegralInputVariableDAG);
                    input->name=iv->name();
                    variables.push_back(input.get());
                    // manage object's lifetime with expressionCache
                    expressionCache.insertIntegralInput(iv->valueId(), input);
                    try
                      {input->rhs=getNodeFromWire(*(i->ports[1]->wires()[0]));}
                    catch (...)
                      {
                        // try again later
                        integralInputs.emplace_back(input,i->ports[1]->wires()[0]);
                      }
                  }
                
                if (i->ports[2]->wires().size()>0)
                  {
                    // second port can be attached to a variable,
                    // which supplies an init string
                    NodePtr init;
                    try
                      {
                        init=getNodeFromWire(*(i->ports[2]->wires()[0]));
                      }
                    catch (...) {}
                    if (auto v=dynamic_cast<VariableDAG*>(init.get()))
                      iv->init(v->name);
                    else if (auto c=dynamic_cast<ConstantDAG*>(init.get()))
                      {
                        // slightly convoluted to prevent sliderSet from overriding c->value
                        iv->init(c->value);
                        iv->adjustSliderBounds();
                      }
                    else
                      throw error("only constants, parameters and variables can be connected to the initial value port");
                  }
                
              }
          }
        return false;
      });

    // add input variables for all stock variables to the expression cache
    minsky.model->recursiveDo
      (&Group::items,
       [&](const Items&, Items::const_iterator it){
        if (auto i=dynamic_cast<Variable<VariableType::stock>*>(it->get()))
          if (!expressionCache.getIntegralInput(i->valueId()))
          {
            VariableDAGPtr input(new IntegralInputVariableDAG);
            input->name=i->name();
            variables.push_back(input.get());
            // manage object's lifetime with expressionCache
            expressionCache.insertIntegralInput(i->valueId(), input);
          }
        return false;
      });
    
    // wire up integral inputs, now that all integrals are defined, so that derivative works. See #511
    for (auto& i: integralInputs)
      i.first->rhs=getNodeFromWire(*i.second);

    // process the Godley tables
    map<string, GodleyColumnDAG> godleyVars;
    m.model->recursiveDo
      (&Group::items,
       [&](const Items&, Items::const_iterator i)
       {
         if (auto g=dynamic_cast<GodleyIcon*>(i->get()))
           processGodleyTable(godleyVars, *g);
         return false;
       });

    for (auto& g: godleyVars)
      {
        //        assert(g->second.godleyId>=0);
        integVarMap[VariableValue::valueId(g.first)]=
          dynamic_cast<VariableDAG*>
          (makeDAG(VariableValue::valueId(g.first),
                   VariableValue::uqName(g.first), VariableValue::stock).get());
        expressionCache.getIntegralInput(g.first)->rhs=
          expressionCache.insertAnonymous(NodePtr(new GodleyColumnDAG(g.second)));
      }

    for (auto& v: integVarMap)
      integrationVariables.push_back(v.second);

    // now start with the variables, and work our way back to how they
    // are defined
    for (VariableValues::value_type v: m.variableValues)
      if (v.second.isFlowVar())
        if (auto vv=dynamic_cast<VariableDAG*>
            (makeDAG(v.first, v.second.name, v.second.type()).get()))
          variables.push_back(vv);
          
    // sort variables into their order of definition
    sort(variables.begin(), variables.end(), 
         VariableDefOrder(expressionCache.size()));
  }

  NodePtr SystemOfEquations::makeDAG(const string& valueId, const string& name, VariableType::Type type)
  {
    if (expressionCache.exists(valueId))
      return expressionCache[valueId];

    assert(VariableValue::isValueId(valueId));
    assert(minsky.variableValues.count(valueId));
    VariableValue vv=minsky.variableValues[valueId];

    if (type==VariableType::constant)
      {
        NodePtr r(new ConstantDAG(vv.init));
        expressionCache.insert(valueId, r);
        return r;
      }

    // ensure name is unique
    string nm=name;
    for (unsigned i=0; varNames.count(nm); ++i)
      nm=name+"_"+to_string(i);
    varNames.insert(nm);
    
    shared_ptr<VariableDAG> r(new VariableDAG(valueId, nm, type));
    expressionCache.insert(valueId, r);
    r->init=vv.init;
    if (auto v=minsky.definingVar(valueId))
      if (v->type()!=VariableType::integral && v->numPorts()>1 && !v->ports[1]->wires().empty())
        r->rhs=getNodeFromWire(*v->ports[1]->wires()[0]);
    return r;
  }

  NodePtr SystemOfEquations::makeDAG(const OperationBase& op)
  {
    if (expressionCache.exists(op))
      return dynamic_pointer_cast<OperationDAGBase>(expressionCache[op]);

    if (op.type()==OperationType::differentiate)
      {
        assert(op.ports.size()==2);
        NodePtr expr;
        if (op.ports[1]->wires().size()==0 || !(expr=getNodeFromWire(*op.ports[1]->wires()[0])))
          {
            minsky::minsky().displayErrorItem(op);          
            throw error("derivative not wired");
          }
        try
          {
            return expressionCache.insert
              (op, expr->derivative(*this));
          }
        catch (...)
          {
            minsky::minsky().displayErrorItem(op);          
            throw;
          }
      }
    else
      {
        shared_ptr<OperationDAGBase> r(OperationDAGBase::create(op.type()));
        expressionCache.insert(op, NodePtr(r));
        r->state=dynamic_pointer_cast<OperationBase>(minsky.model->findItem(op));
        assert(r->state);
        assert( r->state->type()!=OperationType::numOps);

        r->arguments.resize(op.numPorts()-1);
        for (size_t i=1; i<op.ports.size(); ++i)
          {
            auto& p=op.ports[i];
            for (auto w: p->wires())
              r->arguments[i-1].push_back(getNodeFromWire(*w));
          }
        return r;
      }
  }

  NodePtr SystemOfEquations::makeDAG(const SwitchIcon& sw)
  {
    // grab list of input wires
    vector<Wire*> wires;
    for (unsigned i=1; i<sw.ports.size(); ++i)
      {
        auto& w=sw.ports[i]->wires();
        if (w.size()==0)
          {
            minsky.displayErrorItem(sw);
            throw error("input port not wired");
          }
        // shouldn't be more than 1 wire, ignore extraneous wires is present
        assert(w.size()==1);
        wires.push_back(w[0]);
      }

    assert(wires.size()==sw.numCases()+1);
    Expr input(expressionCache, getNodeFromWire(*wires[0]));

    auto r=make_shared<OperationDAG<OperationType::add>>();
    expressionCache.insert(sw, r);
    r->arguments[0].resize(sw.numCases());

    // implemented as a sum of step functions
    r->arguments[0][0]=getNodeFromWire(*wires[1])*(input<1);        
    for (unsigned i=1; i<sw.numCases()-1; ++i)
      r->arguments[0][i]=getNodeFromWire
        (*wires[i+1])*((input<i+1) - (input<i));
    r->arguments[0][sw.numCases()-1]=getNodeFromWire
      (*wires[sw.numCases()])*(1-(input<sw.numCases()-1));
    return r;
  };

  NodePtr SystemOfEquations::getNodeFromWire(const Wire& wire)
  {
    NodePtr r;
    if (auto p=wire.from())
      {
        auto& item=p->item;
        if (auto o=dynamic_cast<OperationBase*>(&item))
          {
            if (expressionCache.exists(*o))
              return expressionCache[*o];
            else
              // we're wired to an operation
              r=makeDAG(*o);
          }
        else if (auto s=dynamic_cast<SwitchIcon*>(&item))
          {
            if (expressionCache.exists(*s))
              return expressionCache[*s];
            else
              r=makeDAG(*s);
          }
        else if (auto v=item.variableCast())
          {
            if (expressionCache.exists(*v))
              return expressionCache[*v];
            else
              if (v && v->type()!=VariableBase::undefined) 
                // we're wired to a variable
                r=makeDAG(*v);
          }
      }
    return r;
  }     
  
    
  ostream& SystemOfEquations::latex(ostream& o) const
  {
    o << "\\begin{eqnarray*}\n";
    for (const VariableDAG* i: variables)
      {
        if (dynamic_cast<const IntegralInputVariableDAG*>(i) ||
            i->type==VariableType::constant) continue;
        o << i->latex() << "&=&";
        if (i->rhs) 
          i->rhs->latex(o);
        else
          o<<latexInit(i->init);
        o << "\\\\\n";
      }

    for (const VariableDAG* i: integrationVariables)
      {
        o << mathrm(i->name)<<"(0)&=&"<<latexInit(i->init)<<"\\\\\n";
        o << "\\frac{ d " << mathrm(i->name) << 
          "}{dt} &=&";
        VariableDAGPtr input=expressionCache.getIntegralInput(i->valueId);
        if (input && input->rhs)
          input->rhs->latex(o);
        o << "\\\\\n";
      }
    return o << "\\end{eqnarray*}\n";
  }

  ostream& SystemOfEquations::latexWrapped(ostream& o) const
  {
    o << "\\begin{dgroup*}\n";
    for (const VariableDAG* i: variables)
      {
        if (dynamic_cast<const IntegralInputVariableDAG*>(i)) continue;
        if (i->type==VariableType::constant) continue;
        o<<"\\begin{dmath*}\n";
        o << i->latex() << "=";
        if (i->rhs) 
          i->rhs->latex(o);
        else
          o<<latexInit(i->init);
        o << "\n\\end{dmath*}\n";
      }

    for (const VariableDAG* i: integrationVariables)
      {
        o<<"\\begin{dmath*}\n";
        o << mathrm(i->name)<<"(0)="<<latexInit(i->init)<<"\n\\end{dmath*}\n";
        o << "\\begin{dmath*}\n\\frac{ d " << mathrm(i->name) << 
          "}{dt} =";
        VariableDAGPtr input=expressionCache.getIntegralInput(i->valueId);
        if (input && input->rhs)
          input->rhs->latex(o);
        o << "\\end{dmath*}\n";
      }
    return o << "\\end{dgroup*}\n";
  }

  ostream& SystemOfEquations::matlab(ostream& o) const
  {
    o<<"function f=f(x,t)\n";
    // define names for the components of x for reference
    int j=1;
    for (const VariableDAG*  i: integrationVariables)
      o<<i->matlab()<<"=x("<<j++<<");\n";

    for (const VariableDAG* i: variables)
      {
        if (dynamic_cast<const IntegralInputVariableDAG*>(i) ||
            i->type==VariableType::constant) continue;
        o << i->matlab() << "=";
        if (i->rhs)
          o << i->rhs->matlab();
        else
          o << matlabInit(i->init);
        o<<";\n";
      }

    j=1;
    for (const VariableDAG* i: integrationVariables)
      {
        o << "f("<<j++<<")=";
        VariableDAGPtr input=expressionCache.getIntegralInput(i->valueId);
        if (input && input->rhs)
          input->rhs->matlab(o);
        else
          o<<0;
        o<<";\n";
      }
    o<<"endfunction;\n\n";

    // now write out the initial conditions
    j=1;
    for (const VariableDAG* i: integrationVariables)
      o << "x0("<<j++<<")="<<matlabInit(i->init)<<";\n";
   
    return o;
  }

  void SystemOfEquations::populateEvalOpVector
  (EvalOpVector& equations, vector<Integral>& integrals)
  {
    equations.clear();
    integrals.clear();

    for (VariableDAG* i: variables)
      {
        i->addEvalOps(equations,i->result);
        assert(minsky.variableValues.validEntries());
      }

    for (const VariableDAG* i: integrationVariables)
      {
        string vid=i->valueId;
        integrals.push_back(Integral());
        assert(VariableValue::isValueId(vid));
        assert(minsky.variableValues.count(vid));
        integrals.back().stock=minsky.variableValues[vid];
        integrals.back().operation=dynamic_cast<IntOp*>(i->intOp);
        VariableDAGPtr iInput=expressionCache.getIntegralInput(vid);
        if (iInput && iInput->rhs)
            integrals.back().input=iInput->rhs->addEvalOps(equations);
      }
    assert(minsky.variableValues.validEntries());

    // ensure all variables have their output port's variable value up to date
    minsky.model->recursiveDo
      (&Group::items,
       [&](Items&, Items::iterator i)
       {
         if (auto v=(*i)->variableCast())
           {
             if (v->type()==VariableType::undefined)
               throw error("variable %s has undefined type",v->name().c_str());
             assert(minsky.variableValues.count(v->valueId()));
             if (!v->ports.empty())
               v->ports[0]->setVariableValue(minsky.variableValues[v->valueId()]);
           }
         else if (auto pw=dynamic_cast<PlotWidget*>(i->get()))
           for (auto& port: pw->ports) 
             for (auto w: port->wires())
               // ensure plot inputs are evaluated
               w->from()->setVariableValue(getNodeFromWire(*w)->addEvalOps(equations));
         else if (auto s=dynamic_cast<Sheet*>(i->get()))
           for (auto w: s->ports[0]->wires())
               // ensure sheet inputs are evaluated
               w->from()->setVariableValue(getNodeFromWire(*w)->addEvalOps(equations));

         return false;
       });
  }

  void SystemOfEquations::processGodleyTable
  (map<string, GodleyColumnDAG>& godleyVariables, const GodleyIcon& gi)
  {
    auto& godley=gi.table;
    for (size_t c=1; c<godley.cols(); ++c)
      {
        string colName=stripActive(trimWS(godley.cell(0,c)));
        if (colName=="_" || VariableValue::uqName(colName).empty())
          continue; // ignore empty Godley columns
        // resolve scope
        colName=VariableValue::valueId(gi.group.lock(), colName);
        if (processedColumns.count(colName)) continue; //skip shared columns
        processedColumns.insert(colName);
        GodleyColumnDAG& gd=godleyVariables[colName];
        gd.arguments.resize(2);
        vector<WeakNodePtr>& arguments=gd.arguments[0];
        for (size_t r=1; r<godley.rows(); ++r)
          {
            if (godley.initialConditionRow(r)) continue;
            FlowCoef fc(godley.cell(r,c));
            if (fc.name.empty()) continue;
            //            if (godley.signConventionReversed(c)) fc.coef*=-1;

            VariablePtr v(VariableType::flow, fc.name);
            v->group=gi.group;

            if (abs(fc.coef)==1)
              gd.arguments[fc.coef<0? 1: 0].push_back(WeakNodePtr(makeDAG(*v)));
            else
              {
                OperationDAG<OperationType::multiply>* term=
                  new OperationDAG<OperationType::multiply>;
                gd.arguments[fc.coef<0? 1: 0].push_back
                  (expressionCache.insertAnonymous(NodePtr(term)));

                term->arguments.resize(2);
                term->arguments[0].push_back
                  (expressionCache.insertAnonymous(NodePtr(new ConstantDAG(abs(fc.coef)))));
                term->arguments[1].push_back(WeakNodePtr(makeDAG(*v)));
              }
          }
      }
  }
}


