/*
 * Copyright 2004 Sandia Corporation.
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the
 * U.S. Government. Redistribution and use in source and binary forms, with
 * or without modification, are permitted provided that this Notice and any
 * statement of authorship are reproduced on all copies.
 */

#include "pqAbstractSliderEventTranslator.h"
#include "pqCheckBoxEventTranslator.h"
#include "pqMenuEventTranslator.h"
#include "pqPushButtonEventTranslator.h"
#include "pqSpinBoxEventTranslator.h"

#include "pqEventTranslator.h"

#include <QCoreApplication>

pqEventTranslator::pqEventTranslator()
{
  QCoreApplication::instance()->installEventFilter(this);
}

pqEventTranslator::~pqEventTranslator()
{
  QCoreApplication::instance()->removeEventFilter(this);
  
  for(int i = 0; i != this->translators.size(); ++i)
    delete this->translators[i];
}

void pqEventTranslator::addDefaultWidgetEventTranslators()
{
  addWidgetEventTranslator(new pqAbstractSliderEventTranslator());
  addWidgetEventTranslator(new pqCheckBoxEventTranslator());
  addWidgetEventTranslator(new pqMenuEventTranslator());
  addWidgetEventTranslator(new pqPushButtonEventTranslator());
  addWidgetEventTranslator(new pqSpinBoxEventTranslator());
}

void pqEventTranslator::addWidgetEventTranslator(pqWidgetEventTranslator* Translator)
{
  if(Translator)
    {
    this->translators.push_back(Translator);
    
    QObject::connect(
      Translator,
      SIGNAL(recordEvent(QObject*, const QString&, const QString&)),
      this,
      SLOT(onRecordEvent(QObject*, const QString&, const QString&)));
    }
}

bool pqEventTranslator::eventFilter(QObject* Object, QEvent* Event)
{
  // If the object doesn't have a name, don't bother ...
  if(Object->objectName().isEmpty())
    return false;

  // Look for a translator for this object ...
  for(int i = 0; i != this->translators.size(); ++i)\
    {
    if(this->translators[i]->translateEvent(Object, Event))
      return false;
    }
    
  return false;
}

void pqEventTranslator::onRecordEvent(QObject* Object, const QString& Command, const QString& Arguments)
{
  QString name = Object->objectName();
  
/*
  for(QObject* parent = Object->parent(); parent; parent = parent->parent())
    name = parent->objectName() + "/" + name;
*/
  
  emit recordEvent(name, Command, Arguments);
}

