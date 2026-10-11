# Pubblicazione di Ticket Manager

**Italiano** | [English](../en/tehnical/ticket-reverse-proxy.md)

Ticket Manager usa il servizio Compose `tickets`, l'immagine `punto/tickets` e il
volume persistente `tickets_data`. Nginx Proxy Manager inoltra le richieste a
`http://tickets:8083` sulla rete `private_app`; l'alias `ticket-app` è equivalente.
La porta 8083 non viene pubblicata sull'host. Con l'overlay Keycloak il servizio
ha anche la rete privata `auth_api`, utilizzata per il backchannel OIDC.

## Indirizzi di questa installazione

- `https://ticket.localhost`: indirizzo locale primario, configurato insieme al
  certificato locale e all'accesso ibrido Keycloak. HTTP reindirizza a HTTPS.
- `https://ticket.tuaazienda.it`: indirizzo pubblico aggiuntivo predisposto su
  richiesta. Richiede ancora DNS e certificato pubblico prima dell'utilizzo.
  La regola HTTP restituisce un redirect verso HTTPS e non serve il login in HTTP.

Il certificato locale appartiene alla CA in `data/keycloak/tls/ca.crt`. La CA deve
essere riconosciuta dal dispositivo che apre il sito; non disabilitare la verifica
del certificato. `localhost` e i suoi sottodomini indicano il computer del browser.

Le credenziali bootstrap Ticket sono nel file locale `ticket-app/.env`, escluso
da Git e con permessi 0600. Le password non vengono riportate nei log di verifica
o nella documentazione. Gli account già presenti non vengono sostituiti dal
bootstrap. Il login aziendale usa le associazioni esplicite definite per Keycloak.

## Origini e sessioni

```dotenv
TICKETS_PUBLIC_ORIGIN=https://ticket.localhost
TICKETS_ADDITIONAL_ORIGINS=https://ticket.tuaazienda.it
```

Sono consentite una origine primaria e al massimo tre aggiuntive, separate da
virgole senza spazi. Ogni origine è esatta: niente percorsi, wildcard o credenziali.
Non si può associare lo stesso Host a due schemi diversi; il passaggio HTTP→HTTPS
va gestito dal proxy. L'overlay `compose.tickets.yaml` passa entrambe le variabili.

L'app verifica Host e Origin, lega ogni sessione all'origine di creazione e imposta
`Secure` per le origini HTTPS. Gli header `X-Forwarded-*` non cambiano queste
decisioni. I dati Ticket sono condivisi fra i domini, mentre le sessioni sono
separate: il cookie di un dominio non autentica richieste dirette all'altro.

Il login OIDC è disponibile soltanto sull'origine di `OIDC_REDIRECT_URI`. In
modalità ibrida, sulle altre origini resta il login locale. Per spostare SSO sul
dominio definitivo occorre aggiornare insieme origine, redirect registrata nel
client Keycloak e configurazione dell'app.

## Provisioning delle regole Nginx

Prima di modificare NPM, eseguire un backup consistente del suo archivio SQLite.
Dopo l'avvio del servizio, usando credenziali NPM valide:

```sh
python3 scripts/configure_ticket_proxy.py \
  --env-file .env.docker --tickets-env-file ticket-app/.env
```

Il comando controlla tutti i conflitti prima di creare regole, conserva gli host
compatibili e non modifica host o certificati esistenti. Non inizializza account
NPM e non emette certificati. Per un'origine HTTPS priva di certificato crea una
regola HTTP con redirect fisso e segnala esplicitamente che HTTPS è ancora da
completare. Una regola presente non dimostra la raggiungibilità da Internet.

Per il dominio pubblico: configurare il DNS verso l'ingresso del server, rendere
raggiungibile Nginx sulle porte previste, installare un certificato valido e
abilitare Force SSL nella regola dedicata. Le porte dei container applicativi e
dei database rimangono private.

## Verifica

La suite usa database temporanei e include workflow, permessi, OIDC, isolamento
delle sessioni fra origini e provisioning idempotente del proxy:

```sh
make test-tickets
```

Sul servizio avviato verificare la catena TLS, il redirect HTTP, la pagina e gli
asset, `/healthz`, il login, le API autenticate, CSRF e logout. La verifica completa
di scrittura può essere svolta su un'istanza temporanea con volume separato; non
cancellare né sostituire il volume operativo per ripulire dati di collaudo.

Per aggiornare un'installazione ibrida conservare tutti gli overlay e i file di
configurazione usati per avviarla. Un avvio con il solo overlay Ticket non mantiene
automaticamente reti, mount e variabili OIDC degli overlay Keycloak.

## Collaudo del 2026-10-11

- Container `projectmanament-tickets-1` sano, modalità `hybrid`, reti
  `private_app` e `auth_api`, nessuna porta pubblicata direttamente.
- NPM: Proxy Host 4 per `ticket.localhost`, inoltro `http://tickets:8083`,
  certificato locale e Force SSL; Proxy Host 6 per `ticket.tuaazienda.it`, stesso
  inoltro e redirect HTTP 308 verso HTTPS, certificato pubblico ancora assente.
- 77 test automatici superati, inclusi workflow di creazione/modifica/commenti,
  persistenza, ruoli, OIDC, origini e configurazione del proxy.
- 35 controlli sul deployment superati: verifica TLS con CA, risorse e cataloghi
  lingua, login locale, sessione, elenchi e filtri, rifiuto CSRF/origini estranee,
  logout, redirect pubblico e raggiungibilità dell'app Punto.
- Apertura nel browser verificata da `http://ticket.localhost` a
  `https://ticket.localhost`, con form locale e accesso aziendale disponibili.
- Nessun ticket o utente di collaudo aggiunto all'archivio operativo. Il dominio
  pubblico è stato verificato localmente con l'header Host; la raggiungibilità
  Internet e il suo HTTPS non sono ancora disponibili.

Backup NPM e configurazione antecedenti alla pubblicazione, log test e rapporto
finale: `backups/ticket-publish-20261011T063230Z/`, directory privata esclusa da Git.
