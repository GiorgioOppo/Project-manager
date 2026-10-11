/* Isolated MySQL 8.4 regression. Requires an empty disposable database and the
   usual DB_* TLS variables. Never point this fixture at an existing workspace. */
#include "accounts.h"
#include "improvements.h"
#include "organization.h"
#include "storage.h"
#include "workspace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Db *db;
static void execute(const char *sql) { assert(db_exec(db,sql,NULL,NULL,NULL)==DB_OK); }
static DbInt scalar(const char *sql) {
    DbStmt *s=NULL;
    assert(db_prepare_v2(db,sql,-1,&s,NULL)==DB_OK && db_step(s)==DB_ROW);
    DbInt n=db_column_int64(s,0); db_finalize(s); return n;
}
static struct json_object *request(const char *method,const char *path,const char *query,const char *body,DbInt actor,int status) {
    WorkspaceResponse r={0};
    assert(improvements_handle(method,path,query,body,actor,&r));
    if (r.status!=status) fprintf(stderr,"%s: expected %d, received %d: %s\n",path,status,r.status,r.body?r.body:"");
    assert(r.status==status);
    struct json_object *o=json_tokener_parse(r.body); workspace_response_free(&r); assert(o); return o;
}
static DbInt value(struct json_object *o,const char *key) {
    struct json_object *v=NULL; assert(json_object_object_get_ex(o,key,&v)); return json_object_get_int64(v);
}
static void discard(struct json_object *o) { json_object_put(o); }
int main(void) {
    assert(storage_preflight("ignored"));
    assert(storage_open("ignored"));
    assert(accounts_open("ignored","admin@schema14.test","IsolatedFixture2026!"));
    assert(db_open_v2("ignored",&db,DB_OPEN_READWRITE,NULL)==DB_OK);
    assert(scalar("SELECT version FROM schema_metadata WHERE id=1")==14);
    execute("INSERT INTO users(id,email,name,role,active,password_hash,salt) VALUES"
            "(2,'area@schema14.test','Area','manager',1,UNHEX(REPEAT('00',32)),UNHEX(REPEAT('00',16))),"
            "(3,'second@schema14.test','Second','manager',1,UNHEX(REPEAT('00',32)),UNHEX(REPEAT('00',16))),"
            "(4,'worker@schema14.test','Worker','worker',1,UNHEX(REPEAT('00',32)),UNHEX(REPEAT('00',16)))");
    execute("INSERT INTO organization_groups(id,name) VALUES(1,'Area')");
    execute("INSERT INTO group_members(group_id,user_id) VALUES(1,2),(1,4)");
    execute("INSERT INTO jobs(id,code,name,status) VALUES(1,'MYSQL14','MySQL fixture','active')");
    execute("INSERT INTO job_group_grants(job_id,group_id,role_id) VALUES(1,1,2)");
    execute("INSERT INTO time_entries(job_id,user_id,owner,date,minutes,note) VALUES(1,4,'worker@schema14.test','2026-10-10',60,'Fixture')");
    execute("INSERT INTO labor_rates(user_id,hourly_cents,currency,starts_on) VALUES(4,6000,'EUR','2026-01-01')");
    assert(organization_open("ignored") && workspace_open("ignored") && improvements_open("ignored"));
    discard(request("POST","/api/improvements/governance/profile-requests","","user_id=1&groups=%5B%22finance%22%5D&revision=1&reason=Fixture",1,200));
    discard(request("POST","/api/improvements/governance/admins","","user_id=3&admin=1&reason=Fixture",1,200));
    struct json_object *o=request("POST","/api/improvements/governance/profile-requests","","user_id=4&groups=%5B%22sales%22%5D&revision=1&reason=Fixture",1,202);
    DbInt profile=value(o,"request_id"); discard(o);
    char path[160]; snprintf(path,sizeof(path),"/api/improvements/governance/profile-requests/%lld/decision",(long long)profile);
    discard(request("POST",path,"","action=approve&reason=Fixture",1,403));
    discard(request("POST",path,"","action=approve&reason=Fixture",3,200));
    discard(request("POST","/api/improvements/governance/delegations","","user_id=2&group_id=1&can_manage_jobs=1&can_manage_groups=1&can_manage_users=1&can_approve_corrections=1&starts_on=2026-01-01&ends_on=2027-12-31&reason=Fixture",1,201));
    assert(access_policy_can_manage_job(db,2,1)==1);
    discard(request("GET","/api/improvements/operations/capacity","from=2026-10-10&to=2026-10-11&user_id=4","",2,200));
    o=request("POST","/api/improvements/integrations/economic-periods","","from=2026-10-10&to=2026-10-10&action=preview",1,200);
    assert(value(o,"cost_cents")==6000);
    struct json_object *hash=NULL; assert(json_object_object_get_ex(o,"preview_hash",&hash));
    char body[512]; snprintf(body,sizeof(body),"from=2026-10-10&to=2026-10-10&action=approve&name=Fixture&reason=Fixture&preview_hash=%s",json_object_get_string(hash)); discard(o);
    discard(request("POST","/api/improvements/integrations/economic-periods","",body,1,201));
    execute("UPDATE labor_rates SET hourly_cents=9000 WHERE user_id=4");
    discard(request("POST","/api/improvements/integrations/budget","","job_id=1&budget_cents=10000&remaining_cents=3000&revision=0&reason=Fixture",1,200));
    o=request("GET","/api/improvements/integrations/budget","job_id=1","",1,200);
    assert(value(o,"actual_cents")==6000 && value(o,"forecast_cents")==9000); discard(o);
    WorkspaceResponse r={0};
    assert(workspace_handle("DELETE","/api/jobs/1","","",1,"admin@schema14.test","manager",&r) && r.status==409); workspace_response_free(&r);
    assert(workspace_handle("POST","/api/time-entries","","job_id=1&date=2026-10-10&minutes=30",4,"worker@schema14.test","worker",&r) && r.status==409); workspace_response_free(&r);
    improvements_close(); workspace_close(); organization_close(); accounts_close(); storage_close();
    assert(storage_open("ignored"));
    assert(scalar("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' AND entity_id=14")==1);
    assert(scalar("SELECT COUNT(*) FROM access_space_admins")==2);
    storage_close(); db_close(db);
    puts("MySQL schema14, scoped delegation, separate approval, frozen costs, protected deletion and restart passed");
    return 0;
}
