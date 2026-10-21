// Area tool: a pocket with one thin diamond island, half-tip angle A degrees,
// half-length 15 mm, turned by R degrees. Click the pocket: expect pocket +
// island selected and nothing added.
#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include <QApplication>
#include <QTest>
#include <cstdio>
#include <cmath>
using namespace c2d;
int main(int argc,char**argv){
  QApplication app(argc,argv);
  int bad=0;
  for(double A: {1.0,1.5,2.0,2.5,3.0,4.0,5.0,6.0,8.0})
  for(double R: {0.0,8.0,30.0,45.0,77.0}){
    Document doc; Canvas cv; cv.resize(900,700); cv.show(); cv.setDocument(&doc); cv.resetTransform(); cv.scale(3,-3); cv.centerOn(QPointF(100,100));
    const QJsonObject L=doc.defaultLayer();
    Element pocket=Element::makeRectangle({100,100},120,100,L);
    const double s=15, h=s*std::tan(A*M_PI/180), a=R*M_PI/180;
    const QPointF c(80,100), d(std::cos(a),std::sin(a)), n(-std::sin(a),std::cos(a));
    Element isl=Element::makePath({c-d*s,c+n*h,c+d*s,c-n*h},true,L);
    doc.addElement(pocket); doc.addElement(isl); cv.rebuild(); cv.setTool(Canvas::SelectArea);
    const int n0=doc.elements().size();
    QTest::mouseClick(cv.viewport(),Qt::LeftButton,Qt::NoModifier,cv.mapFromScene(QPointF(140,100)));
    QStringList sel=cv.selectedElementIds(); sel.sort(); QStringList exp{pocket.id,isl.id}; exp.sort();
    const bool ok= sel==exp && (int)doc.elements().size()==n0;
    // second canvas state: click inside the island alone
    {
      Document d2; Canvas c2; c2.resize(900,700); c2.show(); c2.setDocument(&d2); c2.resetTransform(); c2.scale(3,-3); c2.centerOn(QPointF(100,100));
      Element p2=pocket, i2=isl; d2.addElement(p2); d2.addElement(i2); c2.rebuild(); c2.setTool(Canvas::SelectArea);
      const int m0=d2.elements().size();
      QTest::mouseClick(c2.viewport(),Qt::LeftButton,Qt::NoModifier,c2.mapFromScene(c));
      if(c2.selectedElementIds()!=QStringList{i2.id} || (int)d2.elements().size()!=m0){++bad; printf("FAIL island click tip %.1f turned %.0f: %d added\n",A,R,(int)d2.elements().size()-m0);}
    }
    if(!ok){++bad; printf("FAIL tip %.1f deg (point %.1f deg) turned %.0f: island %s, %d element(s) added\n",A,2*A,R,sel.contains(isl.id)?"selected":"not selected",(int)doc.elements().size()-n0);}
  }
  printf("bad %d\n",bad);
  return bad?1:0;
}
