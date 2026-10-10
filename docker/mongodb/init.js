// Executed once, on the loopback-only initialization server, before auth starts.
const database = process.env.DOC_DB_NAME;
const username = process.env.DOC_DB_USER;
const password = process.env.DOC_DB_PASSWORD;
if (!/^[A-Za-z0-9_]{1,64}$/.test(database || "") ||
    !/^[A-Za-z0-9_]{1,64}$/.test(username || "") || !password) {
  throw new Error("Configurazione dell'account documentale non valida.");
}
db.getSiblingDB(database).createUser({
  user: username,
  pwd: password,
  roles: [{ role: "readWrite", db: database }],
  mechanisms: ["SCRAM-SHA-256"]
});
