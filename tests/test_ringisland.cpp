// The Area tool in a pocket around a one-element multi-ring island (text):
// the letter counters must stay out of the new path. Also a one-ring "O"
// with a hair slit and a horseshoe whose tips nearly meet: their counter and
// room stay uncut.
#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/vectorops.h"
#include <QApplication>
#include <cmath>
#include <cstdio>
using namespace c2d;
static QPainterPath eo(const QVector<QPainterPath> &ps){QPainterPath r; r.setFillRule(Qt::OddEvenFill);
 for(auto&p:ps) for(const QPolygonF&g:p.toSubpathPolygons(QTransform::fromScale(50,50))) r.addPolygon(QTransform::fromScale(0.02,0.02).map(g)); return r;}
static QVector<QPointF> keyhole(double gap){const double R=10,r=6;int n=200; double ao=std::asin(gap/2/R), ai=std::asin(gap/2/r); QVector<QPointF> p; for(int i=0;i<=n;i++){double a=ao+(2*M_PI-2*ao)*i/n;p<<QPointF(50+R*cos(a),50+R*sin(a));} for(int i=0;i<=n;i++){double a=2*M_PI-ai-(2*M_PI-2*ai)*i/n;p<<QPointF(50+r*cos(a),50+r*sin(a));} return p;}
static QVector<QPointF> horseshoe(double gap){double g=gap/2; QVector<QPointF> p{{0,0},{10,0},{10,10},{5+g,10},{5+g,8},{8,8},{8,2},{2,2},{2,8},{5-g,8},{5-g,10},{0,10}}; for(auto&q:p)q+=QPointF(20,20); return p;}
int main(int argc,char**argv){
  QApplication app(argc,argv); int bad=0;
  for (QString t : {"O","8","@","B","o"}) {
    Document doc; Canvas cv; cv.resize(900,700); cv.setDocument(&doc);
    const QJsonObject L=doc.defaultLayer();
    Element pocket=Element::makeRectangle({100,100},160,140,L);
    Element txt=Element::makeText(t,{80,80},40,"DejaVu Sans",L);
    doc.addElement(pocket); doc.addElement(txt); cv.rebuild();
    const int n0=doc.elements().size();
    cv.selectAreaAt({30,40});
    QStringList sel=cv.selectedElementIds();
    QVector<QPainterPath> sp; for(auto&e:doc.elements()) if(sel.contains(e.id)) sp<<e.painterPath;
    QPainterPath S=eo(sp);
    // a point in the text's counter (centre of the glyph's bounding box for O; scan for any counter point)
    QPainterPath tp=eo({txt.painterPath}); QRectF bb=tp.boundingRect(); int cut=0;
    for(double x=bb.left();x<bb.right();x+=0.2) for(double y=bb.top();y<bb.bottom();y+=0.2){QPointF q(x,y); if(!tp.contains(q)&&S.contains(q)&&!QPainterPath(pocket.painterPath).isEmpty()){ // q is in a counter or outside text
        // counter: inside the glyph's outer ring = inside face of fillFace? use fillFace at q
        QVector<QPainterPath> cl{pocket.painterPath,txt.painterPath}; auto f=vec::fillFace(cl,q); QPainterPath fp; fp.setFillRule(Qt::OddEvenFill); for(auto&g:f){fp.addPolygon(g);fp.closeSubpath();}
        if(!fp.contains(QPointF(30,40))) ++cut; }}
    printf("'%s': selected %d (text %s), added %d, counter area also cut: %.1f mm2\n",qPrintable(t),(int)sel.size(),sel.contains(txt.id)?"yes":"no",(int)doc.elements().size()-n0,cut*0.04);
    if(cut) ++bad;
  }
  {
    Document doc; Canvas cv; cv.resize(900,700); cv.setDocument(&doc);
    const QJsonObject L=doc.defaultLayer();
    Element panel=Element::makeRectangle({50,50},100,100,L);
    Element o=Element::makePath(keyhole(0.005),true,L);
    Element h=Element::makePath(horseshoe(0.01),true,L);
    doc.addElement(panel); doc.addElement(o); doc.addElement(h); cv.rebuild();
    cv.selectAreaAt({5,5});
    QStringList sel=cv.selectedElementIds();
    QVector<QPainterPath> sp; for(auto&e:doc.elements()) if(sel.contains(e.id)) sp<<e.painterPath;
    QPainterPath S=eo(sp);
    const bool cuts=S.contains(QPointF(50,50))||S.contains(QPointF(25,25));
    printf("self-near O and horseshoe: selected %d, counter or room cut: %d, panel cut: %d\n",(int)sel.size(),cuts,S.contains(QPointF(5,5)));
    if(cuts||!S.contains(QPointF(5,5))) ++bad;
  }
  return bad?1:0;
}
