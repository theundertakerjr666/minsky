/*
  @copyright Steve Keen 2018
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

#ifndef RAVELWRAP_H
#define RAVELWRAP_H

#include "operation.h"
#include "cairoRenderer.h"

namespace minsky 
{
  class Ravel: public ItemT<Ravel, Operation<OperationType::ravel>>
  {
  public:
    struct RavelImpl;
    struct DataCube;
  private:
    Exclude<RavelImpl*> ravel=nullptr;
    Exclude<DataCube*> dataCube=nullptr;
    void noRavelSetup();
    /// position of the "move" handle, as a proportion of radius
    const double moveX=0.5, moveY=0.5, moveSz=0.1;
    std::string m_filename;
  public:
    Ravel();
    ~Ravel();
    const char* ravelVersion() const; ///< Ravel version string
    const char* lastErr() const;
    void draw(cairo_t* cairo) const override;
    void resize(const LassoBox&) override;
    ClickType::Type clickType(float x, float y) override;
    void onMouseDown(float x, float y);
    void onMouseUp(float x, float y);
    bool onMouseMotion(float x, float y);
    bool onMouseOver(float x, float y);
    void onMouseLeave();
    void loadFile(const std::string&);
    const string& filename() const {return m_filename;}
    void loadDataFromSlice(VariableValue&);
//    const char* toXML() const;
//    void fromXML(const std::string&);

    // representing the state of the handles
    struct HandleState
    {
      double x,y; ///< handle tip coordinates (only angle important, not length)
      size_t sliceIndex, sliceMin, sliceMax;
      bool collapsed, displayFilterCaliper;
      enum ReductionOp {sum, prod, av, stddev, min, max};
      ReductionOp reductionOp;
    };

    struct State
    {
      std::map<std::string, HandleState> handleStates;
      std::vector<std::string> outputHandles;
    };

    /// get the current state of the Ravel
    State getState() const;
    /// apply the \a state to the Ravel, leaving data, slicelabels etc unchanged
    void applyState(const State&);
  };
}

//#ifdef CLASSDESC
//#pragma omit pack minsky::Ravel
//#pragma omit unpack minsky::Ravel
//#endif
//
//namespace classdesc_access
//{
//  template <> struct access_pack<minsky::Ravel>: 
//    public access_pack<minsky::Operation<minsky::OperationType::ravel>> {};
//  template <> struct access_unpack<minsky::Ravel>: 
//    public access_unpack<minsky::Operation<minsky::OperationType::ravel>> {};
//}
#include "ravelWrap.cd"
#include "ravelWrap.xcd"

#endif

