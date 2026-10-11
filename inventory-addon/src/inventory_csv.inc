/* CSV transfer stays inside this service: preview writes are always rolled back. */
#define CSV_LIMIT (256 * 1024)
#define CSV_ROWS 200
#define CSV_COLUMNS 16
typedef struct { char *cells[CSV_COLUMNS]; } CsvRecord;
typedef struct {
    CsvRecord header, records[CSV_ROWS];
    size_t columns, count;
} CsvTable;
typedef struct { char *data; size_t length, capacity; } CsvBuffer;
static const char *const csv_columns[CSV_COLUMNS] = {
    "id","asset_tag","name","category","serial_number","manufacturer","model","status",
    "location","assignee","purchase_date","warranty_until","notes","asset_type","details","version"
};
static void csv_table_free(CsvTable *table) {
    for (size_t c=0;c<CSV_COLUMNS;c++) free(table->header.cells[c]);
    for (size_t r=0;r<table->count;r++)
        for (size_t c=0;c<CSV_COLUMNS;c++) free(table->records[r].cells[c]);
    free(table);
}
static bool csv_append(CsvBuffer *buffer, const char *value, size_t length) {
    if (length > SIZE_MAX-buffer->length-1) return false;
    size_t needed = buffer->length+length+1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 1024;
        while (capacity < needed) {
            if (capacity > SIZE_MAX/2) { capacity=needed; break; }
            capacity *= 2;
        }
        char *next = realloc(buffer->data,capacity);
        if (!next) return false;
        buffer->data=next; buffer->capacity=capacity;
    }
    memcpy(buffer->data+buffer->length,value,length);
    buffer->length += length;
    buffer->data[buffer->length]=0;
    return true;
}
static bool csv_formula_prefix(const char *value) {
    while (*value==' ' || *value=='\t' || *value=='\r' || *value=='\n') value++;
    return *value && strchr("=+-@",*value);
}
/* Doubling an existing leading apostrophe makes the safety prefix reversible. */
static bool csv_write_cell(CsvBuffer *buffer, const char *value, bool text) {
    if (!csv_append(buffer,"\"",1)) return false;
    if (text && (*value=='\'' || csv_formula_prefix(value)) && !csv_append(buffer,"'",1)) return false;
    const char *start=value;
    for (const char *p=value;;p++) {
        if (*p=='\"' || !*p) {
            if (!csv_append(buffer,start,(size_t)(p-start))) return false;
            if (!*p) break;
            if (!csv_append(buffer,"\"\"",2)) return false;
            start=p+1;
        }
    }
    return csv_append(buffer,"\"",1);
}
static void csv_decode_safe_text(char *value) {
    if (value[0]=='\'' && (value[1]=='\'' || csv_formula_prefix(value+1)))
        memmove(value,value+1,strlen(value));
}
static CsvTable *csv_parse(const char *csv, const char **message) {
    size_t length=strlen(csv);
    *message="CSV non valido: usare virgole, intestazione e virgolette doppie per i campi complessi.";
    if (!length || length>CSV_LIMIT || !utf8_text(csv,true)) {
        *message="Il CSV deve essere UTF-8 e non superare 256 KiB.";
        return NULL;
    }
    const char *p=csv;
    if (length>=3 && (unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf) p+=3;
    CsvTable *table=calloc(1,sizeof(*table));
    char *cell=malloc(length+1);
    if (!table || !cell) { free(table); free(cell); return NULL; }
    bool header=true;
    while (*p) {
        if (!header && table->count>=CSV_ROWS) { *message="Importare al massimo 200 righe per volta."; goto invalid; }
        CsvRecord *record=header ? &table->header : &table->records[table->count++];
        size_t columns=0;
        while (true) {
            if (columns>=CSV_COLUMNS) goto invalid;
            size_t used=0;
            if (*p=='\"') {
                p++;
                bool closed=false;
                while (*p) {
                    if (*p=='\"') {
                        if (p[1]=='\"') { cell[used++]='\"'; p+=2; }
                        else { p++; closed=true; break; }
                    } else cell[used++]=*p++;
                }
                if (!closed || (*p && *p!=',' && *p!='\r' && *p!='\n')) goto invalid;
            } else {
                while (*p && *p!=',' && *p!='\r' && *p!='\n') {
                    if (*p=='\"') goto invalid;
                    cell[used++]=*p++;
                }
            }
            cell[used]=0;
            record->cells[columns]=strdup(cell);
            if (!record->cells[columns++]) goto invalid;
            if (*p==',') { p++; continue; }
            if (*p=='\r') { if (p[1]!='\n') goto invalid; p+=2; }
            else if (*p=='\n') p++;
            break;
        }
        if (header) {
            table->columns=columns;
            bool tag=false,name=false;
            for (size_t c=0;c<columns;c++) {
                bool known=false;
                for (size_t n=0;n<CSV_COLUMNS;n++)
                    if (!strcmp(table->header.cells[c],csv_columns[n])) known=true;
                if (!known) { *message="Intestazione CSV sconosciuta."; goto invalid; }
                for (size_t earlier=0;earlier<c;earlier++)
                    if (!strcmp(table->header.cells[earlier],table->header.cells[c])) {
                        *message="Intestazione CSV duplicata."; goto invalid;
                    }
                if (!strcmp(table->header.cells[c],"asset_tag")) tag=true;
                if (!strcmp(table->header.cells[c],"name")) name=true;
            }
            if (!tag || !name) { *message="Il CSV deve contenere asset_tag e name."; goto invalid; }
            header=false;
        } else {
            if (columns!=table->columns) { *message="Il numero di colonne deve coincidere con l'intestazione."; goto invalid; }
            for (size_t c=0;c<columns;c++)
                if (strcmp(table->header.cells[c],"id") && strcmp(table->header.cells[c],"version"))
                    csv_decode_safe_text(record->cells[c]);
        }
    }
    free(cell);
    if (header || !table->count) {
        *message="Il CSV deve contenere un'intestazione e almeno una riga di dati.";
        csv_table_free(table); return NULL;
    }
    return table;
invalid:
    free(cell); csv_table_free(table); return NULL;
}
static const char *csv_cell(const CsvTable *table, size_t row, const char *name) {
    for (size_t c=0;c<table->columns;c++)
        if (!strcmp(table->header.cells[c],name)) return table->records[row].cells[c];
    return "";
}
static bool csv_has(const CsvTable *table, const char *name) {
    for (size_t c=0;c<table->columns;c++) if (!strcmp(table->header.cells[c],name)) return true;
    return false;
}
static bool csv_form_add(Form *form, const char *key, const char *value) {
    if (form->count>=FIELD_COUNT) return false;
    Field *f=&form->fields[form->count++];
    f->key=strdup(key); f->value=strdup(value);
    return f->key && f->value;
}
static bool csv_asset_form(Form *form, json_object *asset, sqlite3_int64 actor, sqlite3_int64 version) {
    memset(form,0,sizeof(*form));
    char number[32];
    for (size_t n=0;n<12;n++) if (!csv_form_add(form,names[n],jstr(asset,names[n]))) goto fail;
    if (!csv_form_add(form,"asset_type",jstr(asset,"asset_type"))) goto fail;
    json_object *details=NULL;
    if (!json_object_object_get_ex(asset,"details",&details) || !details ||
        !csv_form_add(form,"details",json_object_to_json_string_ext(details,JSON_C_TO_STRING_PLAIN))) goto fail;
    snprintf(number,sizeof(number),"%lld",(long long)actor);
    if (!csv_form_add(form,"actor_id",number)) goto fail;
    if (version) {
        snprintf(number,sizeof(number),"%lld",(long long)version);
        if (!csv_form_add(form,"version",number)) goto fail;
    }
    return true;
fail:
    form_free(form); return false;
}
static json_object *csv_existing(const char *tag, bool *error) {
    sqlite3_stmt *s=prepare("SELECT " ASSET_COLUMNS " FROM assets WHERE asset_tag=? COLLATE NOCASE");
    if (!s) { *error=true; return NULL; }
    sqlite3_bind_text(s,1,tag,-1,SQLITE_TRANSIENT);
    int result=sqlite3_step(s);
    json_object *asset=result==SQLITE_ROW ? asset_row(s) : NULL;
    if ((result==SQLITE_ROW && !asset) || (result!=SQLITE_ROW && result!=SQLITE_DONE)) *error=true;
    sqlite3_finalize(s); return asset;
}
static json_object *csv_item(const CsvTable *table, size_t row, const char *action, const char *error) {
    json_object *item=json_object_new_object(), *errors=json_object_new_array();
    addint(item,"row",(sqlite3_int64)row+2);
    /* Keep malformed input bounded even when the row itself fails validation. */
    const char *tag=csv_cell(table,row,"asset_tag"), *name=csv_cell(table,row,"name");
    addstr(item,"asset_tag",strlen(tag)<=64 ? tag : "");
    addstr(item,"name",strlen(name)<=160 ? name : "");
    const char *type=csv_cell(table,row,"asset_type");
    addstr(item,"asset_type",strlen(type)<=32 ? (*type ? type : "it_device") : "");
    addstr(item,"action",action);
    if (error) json_object_array_add(errors,json_object_new_string(error));
    json_object_object_add(item,"errors",errors);
    return item;
}
/* Resolve license references before a dry-run can create temporary software IDs. */
static bool csv_software_references(const CsvTable *table, sqlite3_int64 references[CSV_ROWS], bool invalid[CSV_ROWS]) {
    for (size_t row=0;row<table->count;row++) {
        bool error=false;
        json_object *existing=csv_existing(csv_cell(table,row,"asset_tag"),&error),*details=NULL;
        const char *type=csv_has(table,"asset_type") ? csv_cell(table,row,"asset_type") : existing ? jstr(existing,"asset_type") : "it_device";
        if (!strcmp(type,"license")) {
            if (csv_has(table,"details")) details=strict_details(csv_cell(table,row,"details"));
            else if (existing) {
                json_object *current=NULL;
                if (json_object_object_get_ex(existing,"details",&current)) details=json_object_get(current);
            }
            sqlite3_int64 software_id=details ? jint(details,"software_id") : 0;
            if (software_id>0) {
                json_object *software=load_asset(software_id,&error);
                if (software && !strcmp(jstr(software,"asset_type"),"software")) references[row]=software_id;
                else invalid[row]=true;
                if (software) json_object_put(software);
            }
        }
        if (details) json_object_put(details);
        if (existing) json_object_put(existing);
        if (error) return false;
    }
    return true;
}
static json_object *csv_preview(Form *form, int *status) {
    static const char *const allowed[]={"csv","mode","actor_id"};
    sqlite3_int64 actor;
    const char *mode=field(form,"mode");
    if (!only_fields(form,allowed,3) || !integer(field(form,"actor_id"),1,INT64_MAX,&actor) ||
        (strcmp(mode,"create_only") && strcmp(mode,"upsert")))
        return failure(status,400,"Modalità o autore dell'importazione non validi.");
    const char *message=NULL;
    CsvTable *table=csv_parse(field(form,"csv"),&message);
    if (!table) return failure(status,400,message);
    if (!execute("BEGIN IMMEDIATE")) { csv_table_free(table); return failure(status,503,"Archivio inventario temporaneamente occupato."); }
    sqlite3_int64 software_references[CSV_ROWS]={0};
    bool invalid_references[CSV_ROWS]={0};
    if (!csv_software_references(table,software_references,invalid_references)) {
        (void)execute("ROLLBACK"); csv_table_free(table);
        return failure(status,500,"Impossibile verificare i riferimenti software del CSV.");
    }
    json_object *o=ok_json(), *items=json_object_new_array(), *plan=json_object_new_array();
    json_object_object_add(o,"items",items);
    size_t created=0,updated=0,errors=0;
    bool fatal=false;
    for (size_t r=0;r<table->count;r++) {
        const char *tag=csv_cell(table,r,"asset_tag");
        const char *row_error=NULL;
        for (size_t other=0;other<table->count;other++)
            if (other!=r && !strcasecmp(tag,csv_cell(table,other,"asset_tag"))) row_error="Etichetta duplicata nel CSV.";
        if (!row_error && invalid_references[r])
            row_error="La licenza deve riferirsi a un software già presente prima dell'anteprima.";
        bool db_error=false;
        json_object *existing=row_error ? NULL : csv_existing(tag,&db_error);
        sqlite3_int64 id=existing ? jint(existing,"id") : 0, version=0;
        if (db_error) fatal=true;
        if (existing && !strcmp(mode,"create_only")) row_error="Etichetta inventario già presente.";
        if (existing && !row_error && (!integer(csv_cell(table,r,"version"),1,INT64_MAX-1,&version) || version!=jint(existing,"version")))
            row_error="Versione CSV assente o diversa da quella corrente.";
        sqlite3_int64 supplied_version=0;
        if (*csv_cell(table,r,"version") && !integer(csv_cell(table,r,"version"),1,INT64_MAX-1,&supplied_version))
            row_error="Versione CSV non valida.";
        sqlite3_int64 advisory=0;
        const char *id_text=csv_cell(table,r,"id");
        if (*id_text && !integer(id_text,1,INT64_MAX,&advisory)) row_error="Identificativo CSV non valido.";
        if (!strcmp(mode,"upsert") && *id_text && !row_error) {
            if (existing && advisory!=id) row_error="Identificativo CSV diverso da quello dell'etichetta esistente.";
            else if (!existing) {
                json_object *by_id=load_asset(advisory,&db_error);
                if (by_id) { row_error="L'identificativo CSV appartiene a un'altra etichetta."; json_object_put(by_id); }
                if (db_error) fatal=true;
            }
        }
        Form row_form={0};
        if (!row_error && !fatal) {
            for (size_t n=0;n<12;n++) {
                const char *value=csv_has(table,names[n]) ? csv_cell(table,r,names[n]) : existing ? jstr(existing,names[n]) : "";
                if (!csv_form_add(&row_form,names[n],value)) fatal=true;
            }
            if (csv_has(table,"asset_type") && !csv_form_add(&row_form,"asset_type",csv_cell(table,r,"asset_type"))) fatal=true;
            if (csv_has(table,"details") && !csv_form_add(&row_form,"details",csv_cell(table,r,"details"))) fatal=true;
            char actor_text[32],version_text[32];
            snprintf(actor_text,sizeof(actor_text),"%lld",(long long)actor);
            if (!csv_form_add(&row_form,"actor_id",actor_text)) fatal=true;
            if (id) {
                snprintf(version_text,sizeof(version_text),"%lld",(long long)version);
                if (!csv_form_add(&row_form,"version",version_text)) fatal=true;
            }
        }
        json_object *saved=NULL;
        if (!row_error && !fatal) {
            int save_status=200;
            saved=save_asset(id,&row_form,&save_status);
            if (save_status>=500) fatal=true;
            else if (save_status>=400) row_error=jstr(saved,"error");
        }
        if (!fatal) {
            json_object *item=csv_item(table,r,row_error ? "error" : id ? "update" : "create",row_error);
            if (row_error) errors++;
            else {
                json_object *asset=NULL,*entry=json_object_new_object();
                json_object_object_get_ex(saved,"asset",&asset);
                addstr(item,"asset_type",jstr(asset,"asset_type"));
                addint(entry,"id",id); addint(entry,"expected_version",version);
                if (!strcmp(jstr(asset,"asset_type"),"license")) addint(entry,"software_reference_id",software_references[r]);
                json_object_object_add(entry,"asset",json_object_get(asset));
                json_object_array_add(plan,entry);
                if (id) updated++; else created++;
            }
            json_object_array_add(items,item);
        }
        if (saved) json_object_put(saved);
        if (existing) json_object_put(existing);
        form_free(&row_form);
        if (fatal) break;
    }
    bool rolled_back=execute("ROLLBACK");
    if (fatal || !rolled_back) {
        csv_table_free(table); json_object_put(o); json_object_put(plan);
        return failure(status,500,"Verifica CSV interrotta; nessuna modifica applicata.");
    }
    json_object *totals=json_object_new_object();
    addint(totals,"total",(sqlite3_int64)table->count); addint(totals,"create",(sqlite3_int64)created);
    addint(totals,"update",(sqlite3_int64)updated); addint(totals,"errors",(sqlite3_int64)errors);
    json_object_object_add(o,"summary",totals);
    json_object_object_add(o,"can_commit",json_object_new_boolean(!errors));
    json_object_object_add(o,"preview_id",NULL);
    csv_table_free(table);
    if (strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN))>RESPONSE_LIMIT-64) {
        json_object_put(o); json_object_put(plan);
        return failure(status,413,"Anteprima CSV troppo grande; dividere il file in gruppi più piccoli.");
    }
    if (!errors) {
        unsigned char random[16]={0}; char preview_id[33];
        bool stored=RAND_bytes(random,sizeof(random))==1;
        for (size_t n=0;n<sizeof(random);n++) snprintf(preview_id+n*2,3,"%02x",random[n]);
        OPENSSL_cleanse(random,sizeof(random));
        time_t now=time(NULL);
        sqlite3_stmt *cleanup=prepare("DELETE FROM inventory_import_previews WHERE expires_at<?");
        if (!cleanup) stored=false;
        if (cleanup) { sqlite3_bind_int64(cleanup,1,(sqlite3_int64)now-86400); stored=sqlite3_step(cleanup)==SQLITE_DONE && stored; }
        sqlite3_finalize(cleanup);
        sqlite3_stmt *s=stored ? prepare("INSERT INTO inventory_import_previews(preview_id,actor_id,created_at,expires_at,plan) SELECT ?1,?2,?3,?4,?5 WHERE (SELECT COUNT(*) FROM inventory_import_previews WHERE actor_id=?2 AND used=0 AND expires_at>?3)<64") : NULL;
        stored=s && sqlite3_bind_text(s,1,preview_id,-1,SQLITE_TRANSIENT)==SQLITE_OK &&
            sqlite3_bind_int64(s,2,actor)==SQLITE_OK && sqlite3_bind_int64(s,3,(sqlite3_int64)now)==SQLITE_OK &&
            sqlite3_bind_int64(s,4,(sqlite3_int64)now+900)==SQLITE_OK &&
            sqlite3_bind_text(s,5,json_object_to_json_string_ext(plan,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT)==SQLITE_OK &&
            sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1;
        sqlite3_finalize(s);
        if (!stored) { json_object_put(o); json_object_put(plan); return failure(status,503,"Impossibile conservare l'anteprima; riprovare dopo la scadenza delle anteprime aperte."); }
        addstr(o,"preview_id",preview_id);
    }
    json_object_put(plan);
    return o;
}
static json_object *csv_commit(Form *form, int *status) {
    static const char *const allowed[]={"preview_id","actor_id"};
    sqlite3_int64 actor;
    const char *preview_id=field(form,"preview_id");
    bool token=strlen(preview_id)==32;
    for (const char *p=preview_id;*p;p++) if (!(*p>='0' && *p<='9') && !(*p>='a' && *p<='f')) token=false;
    if (!only_fields(form,allowed,2) || !token || !integer(field(form,"actor_id"),1,INT64_MAX,&actor))
        return failure(status,400,"Anteprima o autore dell'importazione non validi.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    sqlite3_stmt *s=prepare("SELECT plan,expires_at,used FROM inventory_import_previews WHERE preview_id=? AND actor_id=?");
    bool valid=s && sqlite3_bind_text(s,1,preview_id,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int64(s,2,actor)==SQLITE_OK;
    int result=valid ? sqlite3_step(s) : SQLITE_ERROR;
    json_object *plan=NULL;
    const char *message="Anteprima assente, scaduta o già utilizzata; verificare nuovamente il CSV.";
    int code=409;
    if (result==SQLITE_ROW && sqlite3_column_int64(s,1)>=(sqlite3_int64)time(NULL) && !sqlite3_column_int(s,2))
        plan=json_tokener_parse((const char *)sqlite3_column_text(s,0));
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) { code=500; message="Impossibile leggere l'anteprima."; }
    sqlite3_finalize(s);
    if (!plan || !json_object_is_type(plan,json_type_array)) {
        if (plan) json_object_put(plan);
        (void)execute("ROLLBACK"); return failure(status,code,message);
    }
    s=prepare("UPDATE inventory_import_previews SET used=1 WHERE preview_id=? AND actor_id=? AND used=0");
    bool ok=s && sqlite3_bind_text(s,1,preview_id,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int64(s,2,actor)==SQLITE_OK &&
        sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1;
    sqlite3_finalize(s);
    if (!ok || !execute("SAVEPOINT inventory_csv_batch")) {
        json_object_put(plan); (void)execute("ROLLBACK"); return failure(status,500,"Impossibile avviare l'importazione.");
    }
    size_t created=0,updated=0;
    for (size_t r=0;ok && r<json_object_array_length(plan);r++) {
        json_object *entry=json_object_array_get_idx(plan,r), *asset=NULL;
        sqlite3_int64 id=jint(entry,"id"),version=jint(entry,"expected_version");
        json_object_object_get_ex(entry,"asset",&asset);
        if (!strcmp(jstr(asset,"asset_type"),"license")) {
            json_object *details=NULL;
            json_object_object_get_ex(asset,"details",&details);
            if (jint(entry,"software_reference_id")<=0 || jint(entry,"software_reference_id")!=jint(details,"software_id")) {
                ok=false; message="Il riferimento software della licenza richiede una nuova anteprima.";
            }
        }
        bool db_error=false;
        json_object *current=csv_existing(jstr(asset,"asset_tag"),&db_error);
        if (db_error) { ok=false; code=500; message="Impossibile verificare l'inventario corrente."; }
        else if ((!id && current) || (id && (!current || jint(current,"id")!=id || jint(current,"version")!=version))) {
            ok=false; message="L'inventario è cambiato dopo l'anteprima; verificare nuovamente il CSV.";
        }
        if (current) json_object_put(current);
        Form row_form={0};
        if (ok && !csv_asset_form(&row_form,asset,actor,version)) { ok=false; code=500; message="Impossibile preparare l'importazione."; }
        if (ok) {
            int save_status=200;
            json_object *saved=save_asset(id,&row_form,&save_status);
            if (save_status>=400) {
                ok=false; code=save_status>=500 ? 500 : 409;
                message=save_status>=500 ? "Importazione annullata: salvataggio o storico non riusciti." : "I dati correnti non consentono più l'importazione; verificare nuovamente il CSV.";
            } else if (id) updated++; else created++;
            json_object_put(saved);
        }
        form_free(&row_form);
    }
    json_object_put(plan);
    if (!ok) {
        bool reverted=execute("ROLLBACK TO inventory_csv_batch;RELEASE inventory_csv_batch");
        if (!reverted || !execute("COMMIT")) { (void)execute("ROLLBACK"); return failure(status,500,"Importazione annullata; impossibile chiudere la verifica."); }
        return failure(status,code,message);
    }
    if (!execute("RELEASE inventory_csv_batch;COMMIT")) {
        (void)execute("ROLLBACK"); return failure(status,500,"Importazione annullata; transazione non salvata.");
    }
    json_object *o=ok_json(), *totals=json_object_new_object();
    addint(totals,"total",(sqlite3_int64)(created+updated)); addint(totals,"create",(sqlite3_int64)created); addint(totals,"update",(sqlite3_int64)updated);
    json_object_object_add(o,"summary",totals);
    return o;
}
static json_object *csv_export(const char *query, int *status) {
    json_object *page=list_assets(query,status);
    if (*status>=400) return page;
    CsvBuffer csv={0};
    bool ok=true;
    for (size_t n=0;n<CSV_COLUMNS;n++) {
        if (n) ok=csv_append(&csv,",",1) && ok;
        ok=csv_write_cell(&csv,csv_columns[n],false) && ok;
    }
    ok=csv_append(&csv,"\r\n",2) && ok;
    json_object *items=NULL,*next=NULL,*o=ok_json();
    json_object_object_get_ex(page,"items",&items);
    json_object_object_get_ex(page,"next_after_id",&next);
    json_object_object_add(o,"next_after_id",next ? json_object_get(next) : NULL);
    addint(o,"exported",0);
    size_t count=0;
    sqlite3_int64 last=0;
    for (size_t r=0;ok && r<json_object_array_length(items);r++) {
        json_object *asset=json_object_array_get_idx(items,r),*details=NULL;
        json_object_object_get_ex(asset,"details",&details);
        size_t before=csv.length;
        for (size_t c=0;c<CSV_COLUMNS;c++) {
            if (c) ok=csv_append(&csv,",",1) && ok;
            const char *value=!strcmp(csv_columns[c],"details") ? json_object_to_json_string_ext(details,JSON_C_TO_STRING_PLAIN) : jstr(asset,csv_columns[c]);
            ok=csv_write_cell(&csv,value,c!=0 && c!=15) && ok;
        }
        ok=csv_append(&csv,"\r\n",2) && ok;
        addstr(o,"csv",csv.data); addint(o,"exported",(sqlite3_int64)count+1);
        if (strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN))>RESPONSE_LIMIT-64) {
            csv.length=before; csv.data[before]=0;
            addint(o,"next_after_id",last);
            break;
        }
        count++; last=jint(asset,"id");
    }
    if (ok) { addstr(o,"csv",csv.data); addint(o,"exported",(sqlite3_int64)count); }
    free(csv.data); json_object_put(page);
    if (!ok) { json_object_put(o); return failure(status,500,"Esportazione CSV non riuscita."); }
    return o;
}
