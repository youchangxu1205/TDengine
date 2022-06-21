###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

from collections import defaultdict
import random
import string
import threading
import requests
import time
# import socketfrom

import taos
from util.log import *
from util.sql import *
from util.cases import *
from util.dnodes import *
from util.common import *

# class actionType(Enum):
#     CREATE_DATABASE = 0
#     CREATE_STABLE   = 1
#     CREATE_CTABLE   = 2
#     INSERT_DATA     = 3

class TMQCom:
    def init(self, conn, logSql):
        tdSql.init(conn.cursor())
        # tdSql.init(conn.cursor(), logSql)  # output sql.txt file

    def initConsumerTable(self,cdbName='cdb'):        
        tdLog.info("create consume database, and consume info table, and consume result table")
        tdSql.query("create database if not exists %s vgroups 1"%(cdbName))
        tdSql.query("drop table if exists %s.consumeinfo "%(cdbName))
        tdSql.query("drop table if exists %s.consumeresult "%(cdbName))

        tdSql.query("create table %s.consumeinfo (ts timestamp, consumerid int, topiclist binary(1024), keylist binary(1024), expectmsgcnt bigint, ifcheckdata int, ifmanualcommit int)"%cdbName)
        tdSql.query("create table %s.consumeresult (ts timestamp, consumerid int, consummsgcnt bigint, consumrowcnt bigint, checkresult int)"%cdbName)

    def initConsumerInfoTable(self,cdbName='cdb'):        
        tdLog.info("drop consumeinfo table")
        tdSql.query("drop table if exists %s.consumeinfo "%(cdbName))
        tdSql.query("create table %s.consumeinfo (ts timestamp, consumerid int, topiclist binary(1024), keylist binary(1024), expectmsgcnt bigint, ifcheckdata int, ifmanualcommit int)"%cdbName)

    def insertConsumerInfo(self,consumerId, expectrowcnt,topicList,keyList,ifcheckdata,ifmanualcommit,cdbName='cdb'):    
        sql = "insert into %s.consumeinfo values "%cdbName
        sql += "(now, %d, '%s', '%s', %d, %d, %d)"%(consumerId, topicList, keyList, expectrowcnt, ifcheckdata, ifmanualcommit)
        tdLog.info("consume info sql: %s"%sql)
        tdSql.query(sql)

    def selectConsumeResult(self,expectRows,cdbName='cdb'):
        resultList=[]
        while 1:
            tdSql.query("select * from %s.consumeresult"%cdbName)
            #tdLog.info("row: %d, %l64d, %l64d"%(tdSql.getData(0, 1),tdSql.getData(0, 2),tdSql.getData(0, 3))
            if tdSql.getRows() == expectRows:
                break
            else:
                time.sleep(5)
        
        for i in range(expectRows):
            tdLog.info ("consume id: %d, consume msgs: %d, consume rows: %d"%(tdSql.getData(i , 1), tdSql.getData(i , 2), tdSql.getData(i , 3)))
            resultList.append(tdSql.getData(i , 3))
        
        return resultList

    def startTmqSimProcess(self,pollDelay,dbName,showMsg=1,showRow=1,cdbName='cdb',valgrind=0):
        buildPath = tdCom.getBuildPath()
        cfgPath = tdCom.getClientCfgPath()
        if valgrind == 1:
            logFile = cfgPath + '/../log/valgrind-tmq.log'
            shellCmd = 'nohup valgrind --log-file=' + logFile
            shellCmd += '--tool=memcheck --leak-check=full --show-reachable=no --track-origins=yes --show-leak-kinds=all --num-callers=20 -v --workaround-gcc296-bugs=yes '
        
        if (platform.system().lower() == 'windows'):
            shellCmd = 'mintty -h never -w hide ' + buildPath + '\\build\\bin\\tmq_sim.exe -c ' + cfgPath
            shellCmd += " -y %d -d %s -g %d -r %d -w %s "%(pollDelay, dbName, showMsg, showRow, cdbName) 
            shellCmd += "> nul 2>&1 &"   
        else:
            shellCmd = 'nohup ' + buildPath + '/build/bin/tmq_sim -c ' + cfgPath
            shellCmd += " -y %d -d %s -g %d -r %d -w %s "%(pollDelay, dbName, showMsg, showRow, cdbName) 
            shellCmd += "> /dev/null 2>&1 &"
        tdLog.info(shellCmd)
        os.system(shellCmd)

    def getStartConsumeNotifyFromTmqsim(self,cdbName='cdb'):
        while 1:
            tdSql.query("select * from %s.notifyinfo"%cdbName)
            #tdLog.info("row: %d, %l64d, %l64d"%(tdSql.getData(0, 1),tdSql.getData(0, 2),tdSql.getData(0, 3))
            if (tdSql.getRows() == 1) and (tdSql.getData(0, 1) == 0):
                break
            else:
                time.sleep(0.1)
        return

    def getStartCommitNotifyFromTmqsim(self,cdbName='cdb'):
        while 1:
            tdSql.query("select * from %s.notifyinfo"%cdbName)
            #tdLog.info("row: %d, %l64d, %l64d"%(tdSql.getData(0, 1),tdSql.getData(0, 2),tdSql.getData(0, 3))
            if tdSql.getRows() == 2 :
                print(tdSql.getData(0, 1), tdSql.getData(1, 1))
                if tdSql.getData(1, 1) == 1:
                    break
            time.sleep(0.1)
        return

    def insert_data(self,tsql,dbName,stbName,ctbNum,rowsPerTbl,batchNum,startTs):
        tdLog.debug("start to insert data ............")
        tsql.execute("use %s" %dbName)
        pre_insert = "insert into "
        sql = pre_insert

        t = time.time()
        startTs = int(round(t * 1000))
        #tdLog.debug("doing insert data into stable:%s rows:%d ..."%(stbName, allRows))
        for i in range(ctbNum):
            sql += " %s%d values "%(stbName,i)
            for j in range(rowsPerTbl):
                sql += "(%d, %d, 'tmqrow_%d') "%(startTs + j, j, j)
                if (j > 0) and ((j%batchNum == 0) or (j == rowsPerTbl - 1)):
                    tsql.execute(sql)
                    if j < rowsPerTbl - 1:
                        sql = "insert into %s%d values " %(stbName,i)
                    else:
                        sql = "insert into "
        #end sql
        if sql != pre_insert:
            #print("insert sql:%s"%sql)
            tsql.execute(sql)
        tdLog.debug("insert data ............ [OK]")
        return

    def syncCreateDbStbCtbInsertData(self, tsql, paraDict):
        tdCom.create_database(tsql, paraDict["dbName"],paraDict["dropFlag"])
        tdCom.create_stable(tsql, dbname=paraDict["dbName"],stbname=paraDict["stbName"], column_elm_list=paraDict['colSchema'], tag_elm_list=paraDict['tagSchema'])
        tdCom.create_ctable(tsql, dbname=paraDict["dbName"],stbname=paraDict["stbName"],tag_elm_list=paraDict['tagSchema'],count=paraDict["ctbNum"], default_ctbname_prefix=paraDict['ctbPrefix'])
        if "event" in paraDict and type(paraDict['event']) == type(threading.Event()):
            paraDict["event"].set()

        ctbPrefix = paraDict['ctbPrefix']
        ctbNum = paraDict["ctbNum"]
        for i in range(ctbNum):
            tbName = '%s%s'%(ctbPrefix,i)
            tdCom.insert_rows(tsql,dbname=paraDict["dbName"],tbname=tbName,start_ts_value=paraDict['startTs'],count=paraDict['rowsPerTbl'])
        return 

    def threadFunction(self, **paraDict):
        # create new connector for new tdSql instance in my thread
        newTdSql = tdCom.newTdSql()
        self.syncCreateDbStbCtbInsertData(self, newTdSql, paraDict)
        return

    def asyncCreateDbStbCtbInsertData(self, paraDict):
        pThread = threading.Thread(target=self.threadFunction, kwargs=paraDict)
        pThread.start()
        return pThread

    def close(self):
        self.cursor.close()

tmqCom = TMQCom()
