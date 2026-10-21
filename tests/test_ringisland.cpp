// The Area tool in a pocket around a one-element multi-ring island (text):
// the letter counters must stay out of the new path.
#include "../src/canvas.h"
#include "../src/c2ddocument.h"
#include "../src/element.h"
#include "../src/vectorops.h"
#include <QApplication>
#include <cstdio>
using namespace c2d;
static QPainterPath eo(const QVector<QPainterPath> &ps){QPainterPath r; r.setFillRule(Qt::OddEvenFill);
 for(auto&p:ps) for(const QPolygonF&g:p.toSubpathPolygons(QTransform::fromScale(50,50))) r.addPolygon(QTransform::fromScale(0.02,0.02).map(g)); return r;}
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
  return bad?1:0;
}
