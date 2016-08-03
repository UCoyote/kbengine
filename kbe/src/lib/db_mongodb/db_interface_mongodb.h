#pragma once
#include "common.h"
#include "db_transaction.h"
#include "common/common.h"
#include "common/singleton.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "db_interface/db_interface.h"

#include "mongoc.h"
#include <bson.h>
#include <bcon.h>
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma comment (lib, "bson-static_d.lib")
#pragma comment (lib, "mongoc_static_d.lib")
#endif

namespace KBEngine 
{
	class DBInterfaceMongodb : public DBInterface
	{
	public:
		DBInterfaceMongodb(const char* name);
		virtual ~DBInterfaceMongodb();

		static bool initInterface(DBInterface* pdbi);

		/**
		��ĳ�����ݿ����
		*/
		bool reattach();
		virtual bool attach(const char* databaseName = NULL);
		virtual bool detach();

		/*bool ping(){
			return mysql_ping(pMysql_) == 0;
		}*/

		bool ping(mongoc_client_t* pMongoClient = NULL);

		void inTransaction(bool value)
		{
			KBE_ASSERT(inTransaction_ != value);
			inTransaction_ = value;
		}

		bool hasLostConnection() const		{ return hasLostConnection_; }
		void hasLostConnection(bool v)	{ hasLostConnection_ = v; }

		/**
		��黷��
		*/
		virtual bool checkEnvironment();

		/**
		������ �Դ�������ݽ��о���
		����������ɹ�����ʧ��
		*/
		virtual bool checkErrors();

		virtual bool query(const char* strCommand, uint32 size, bool printlog = true, MemoryStream * result = NULL);

		bool write_query_result(MemoryStream * result);

		/**
		��ȡ���ݿ����еı���
		*/
		virtual bool getTableNames(std::vector<std::string>& tableNames, const char * pattern);

		/**
		��ȡ���ݿ�ĳ�������е��ֶ�����
		*/
		virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames);

		/*const char* getLastError()
		{
		if (pMongoClient == NULL)
		return "pMongoClient is NULL";

		return pMongoClient->in_exhaust;
		}*/

		/**
		��������ӿڵ�����
		*/
		virtual const char* c_str();

		/**
		��ȡ����
		*/
		virtual const char* getstrerror();

		/**
		��ȡ������
		*/
		virtual int getlasterror();

		/**
		������ݿⲻ�����򴴽�һ�����ݿ�
		*/
		virtual bool createDatabaseIfNotExist();//�д���֤�Ƿ���Ҫ

		/**
		����һ��entity�洢��
		*/
		virtual EntityTable* createEntityTable(EntityTables* pEntityTables);

		/**
		�����ݿ�ɾ��entity��
		*/
		virtual bool dropEntityTableFromDB(const char* tableName);

		/**
		�����ݿ�ɾ��entity���ֶ�
		*/
		virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName);

		mongoc_client_t* mongo(){ return _pMongoClient; }

		/**
		��ס�ӿڲ���
		*/
		virtual bool lock();
		virtual bool unlock();

		void throwError();

		/**
		�����쳣
		*/
		bool processException(std::exception & e);

	protected:
		mongoc_client_t *_pMongoClient;
		mongoc_collection_t  *collection;
		mongoc_database_t *database;
		bson_t               *command,
			reply,
			*insert;
		bson_error_t          error;
		char                 *str;
		bool                  retval;
		/*mongoc_cursor_t *cursor;
		const bson_t *reply;
		uint16_t port;
		bson_error_t error;
		bson_t b;
		char *host_and_port;
		char *str;*/

		mongodb::DBTransaction lock_;

		bool hasLostConnection_;

		bool inTransaction_;
	};
}