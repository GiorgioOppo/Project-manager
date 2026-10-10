try {
  const application = db.getSiblingDB(process.env.DOC_DB_NAME);
  application.auth(process.env.DOC_DB_USER, process.env.DOC_DB_PASSWORD);
  if (application.runCommand({ ping: 1 }).ok !== 1) quit(1);
  quit(0);
} catch (_) {
  quit(1);
}
