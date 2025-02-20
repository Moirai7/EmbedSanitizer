//===-- Runtime race detection module of EmbedSanitizer - for Embeded ARM--===//
//
//
// This file is distributed under the BSD 3-clause "New" or "Revised" License
// License. See LICENSE.md for details.
//
//===----------------------------------------------------------------------===//
//
// (c) 2017 - 2021  Hassan Salehe Matar, Koc University
//            Email: hmatar@ku.edu.tr
//===----------------------------------------------------------------------===//

// Code for formatting race warning messages

#ifndef __RACE_REPORT__H_
#define __RACE_REPORT__H_

#include <mutex>
#include <iostream>
#include <string>
#include <sstream>
#include <stack>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <set>
#include "race.h"
#include "file_dictionary.h"

// Namespace which contains utility functions for manipulating data
// race reporting metadata.
namespace etsan
{

  static std::mutex racePrintLock;

  static std::mutex stackFrameLock;

  // Temporary location for Race reporting metadata:
  static std::unordered_map<unsigned int, std::vector<char *>> callStack;

  // Keeps list of races
  static std::set<Race, race_compare> races;

  // for printing all races in the list "races"
  void printRaces()
  {
    std::string msg;
    for (auto &race : races)
    {
      race.createRaceMessage(msg);
    }

    racePrintLock.lock();
    printf("EmbedSanitizer: races: %s\n", msg.c_str());// print to standard output
    racePrintLock.unlock();
  }

  // Pushes a function name to a call stack of a thread
  void pushFunction(char *funcName)
  {

    unsigned int tid = (unsigned int)pthread_self();

    stackFrameLock.lock();
    callStack[tid].push_back(funcName);
    printf("EmbedSanitizer: function entry: %s\n", funcName);
    stackFrameLock.unlock();
  }

  void popFunction(char *funcName)
  {

    unsigned int tid = (unsigned int)pthread_self();

    stackFrameLock.lock();
    std::vector<char *> &stackFrame = callStack[tid];
    stackFrameLock.unlock();

    if (stackFrame.size() && stackFrame.back() == funcName)
    {
      stackFrame.pop_back();
    }
    else
    {
      std::cout << "Something wrong with Function Stack: " << funcName << "\n";
    }
  }

  std::vector<char *> getStack(unsigned int tid)
  {
    stackFrameLock.lock();
    std::vector<char *> &stackFrame = callStack[tid];
    stackFrameLock.unlock();
    return stackFrame;
  }

  // Prints the call stack of a thread when a race is found
  std::string printStack()
  {

    unsigned int tid = (unsigned int)pthread_self();
    std::stringstream ss;

    stackFrameLock.lock();
    std::vector<char *> &stackFrame = callStack[tid];
    stackFrameLock.unlock();

    int depth = 1;
    for (char *func : stackFrame)
    {
      std::string msg(depth, ' ');
      depth += 4;
      ss << msg << " '--->" << func << "(...)" << std::endl;
    }

    return ss.str();
  }

  void reportRaceOnRead(int lineNo, void *objName, void *fileName)
  {

    unsigned int tid = (unsigned int)pthread_self();
    Race race(tid, lineNo, "read", (char *)objName, (char *)fileName);
    race.trace = getStack(tid);

    races.insert(race);
    printRaces();
  }

  void reportRaceOnWrite(int lineNo, void *objName, void *fileName)
  {

    unsigned int tid = (unsigned int)pthread_self();
    Race race(tid, lineNo, "write", (char *)objName, (char *)fileName);
    race.trace = getStack(tid);

    races.insert(race);
    printRaces();
  }

} // etsan
#endif
