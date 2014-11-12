#include <bts/mail/client.hpp>
#include <bts/mail/exceptions.hpp>
#include <bts/mail/server.hpp>
#include <bts/db/level_map.hpp>
#include <bts/db/cached_level_map.hpp>
#include <bts/blockchain/time.hpp>

#include <fc/io/buffered_iostream.hpp>
#include <fc/io/json.hpp>
#include <fc/network/tcp_socket.hpp>

#include <queue>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/composite_key.hpp>

#ifndef UNUSED
#define UNUSED(var) ((void)(var))
#endif

namespace bts {
namespace mail {

using namespace fc;
using namespace bts::blockchain;
using namespace bts::wallet;
using std::string;
using namespace boost;
using namespace boost::multi_index;

namespace detail {
#define BTS_MAIL_CLIENT_DATABASE_VERSION 1
#define BTS_MAIL_CLIENT_MAX_INVENTORY_SIZE 1000

struct mail_record {
    mail_record(string sender = string(),
                string recipient = string(),
                public_key_type recipient_key = public_key_type(),
                message&& content = message())
        : id(content.id()),
          status(client::submitted),
          sender(sender),
          recipient(recipient),
          recipient_key(recipient_key),
          content(content)
    {}

    //Static email ID; not to be confused with proof-of-work hash in content.id()
    ripemd160 id;
    client::mail_status status;
    string sender;
    string recipient;
    public_key_type recipient_key;
    message content;
    mail_server_list mail_servers;
    ripemd160 proof_of_work_target;
    string failure_reason;
};

struct mail_archive_record {
    mail_archive_record(mail_record&& from_record = mail_record())
        : id(std::move(from_record.id)),
          status(from_record.status),
          sender(std::move(from_record.sender)),
          recipient(std::move(from_record.recipient)),
          recipient_address(address(std::move(from_record.recipient_key))),
          content(std::move(from_record.content)),
          mail_servers(std::move(from_record.mail_servers))
    {}
    mail_archive_record(message&& from_message, const email_header& header, const address& recipient_address)
        : id(from_message.id()),
          status(client::received),
          sender(header.sender),
          recipient(header.recipient),
          recipient_address(recipient_address),
          content(std::move(from_message))
    {}

    ripemd160 id;
    client::mail_status status;
    string sender;
    string recipient;
    address recipient_address;
    message content;
    mail_server_list mail_servers;
};

struct mail_index_record {
    mail_index_record(const email_header& header)
        : id(header.id),
          sender(header.sender),
          recipient(header.recipient),
          timestamp(header.timestamp)
    {}
    mail_index_record(mail_archive_record&& from_record)
        : id(from_record.id),
          sender(from_record.sender),
          recipient(from_record.recipient),
          timestamp(from_record.content.timestamp)
    {}

    ripemd160 id;
    string sender;
    string recipient;
    fc::time_point_sec timestamp;
};

class client_impl {
protected:
    //Dummy types to act as mnemonics for multi_index indices
    struct sender{};
    struct by_sender{};
    struct recipient{};
    struct by_recipient{};
    struct timestamp{};
    struct by_timestamp{};

public:
    client* self;
    wallet_ptr _wallet;
    chain_database_ptr _chain;
    uint32_t _messages_in;

    typedef std::queue<message_id_type> job_queue;

    job_queue _proof_of_work_jobs;
    fc::future<void> _proof_of_work_worker;

    job_queue _transmit_message_jobs;
    fc::future<void> _transmit_message_worker;
    fc::thread _proof_of_work_thread;

    fc::future<void> _archive_indexing_future;
    fc::thread _archive_indexing_thread;

    bts::db::cached_level_map<message_id_type, mail_record> _processing_db;
    bts::db::level_map<message_id_type, mail_archive_record> _archive;
    bts::db::cached_level_map<message_id_type, email_header> _inbox;
    bts::db::level_map<string, variant> _property_db;

    multi_index_container<
        mail_index_record,
        indexed_by<
            //Allow indexing by message ID
            ordered_unique<
                member<mail_index_record, message_id_type, &mail_index_record::id>
            >,
            //Allow indexing by sender-recipient pair
            //This implicitly allows indexing by sender regardless of recipient
            ordered_non_unique<
                tag<sender,by_sender>,
                composite_key<
                    mail_index_record,
                    member<mail_index_record, string, &mail_index_record::sender>,
                    member<mail_index_record, string, &mail_index_record::recipient>,
                    member<mail_index_record, time_point_sec, &mail_index_record::timestamp>
                >
            >,
            //Allow indexing by recipient, regardless of sender
            ordered_non_unique<
                tag<recipient,by_recipient>,
                composite_key<
                    mail_index_record,
                    member<mail_index_record, string, &mail_index_record::recipient>,
                    member<mail_index_record, time_point_sec, &mail_index_record::timestamp>
                >
            >,
            //Sort based on timestamp
            ordered_non_unique<
                tag<timestamp,by_timestamp>,
                member<mail_index_record, time_point_sec, &mail_index_record::timestamp>
            >
        >
    > _mail_index;

    client_impl(client* self, wallet_ptr wallet, chain_database_ptr chain)
        : self(self),
          _wallet(wallet),
          _chain(chain),
          _proof_of_work_thread("Mail client proof-of-work thread"),
          _archive_indexing_thread("Mail client indexing thread")
    {}
    ~client_impl(){
        _proof_of_work_worker.cancel_and_wait("Mail client destroyed");
        _archive_indexing_future.cancel_and_wait();

        _archive.close();
        _processing_db.close();
        _inbox.close();
        _property_db.close();
    }

    void retry_message(mail_record email) {
        switch (email.status) {
        case client::submitted:
            process_outgoing_mail(email);
            break;
        case client::proof_of_work:
            schedule_proof_of_work(email.id);
            break;
        case client::transmitting:
            schedule_transmit_message(email.id);
            break;
        case client::accepted:
            finalize_message(email.id);
            break;
        default:
            //Do nothing
            break;
        }
    }

    void open(const fc::path& data_dir) {
        try {
            _archive.open(data_dir / "archive");
            _processing_db.open(data_dir / "processing");
            _inbox.open(data_dir / "inbox");
            _property_db.open(data_dir / "properties");

            if (!_property_db.fetch_optional("version"))
                _property_db.store("version", BTS_MAIL_CLIENT_DATABASE_VERSION);

            if (_property_db.fetch("version").as_int64() != BTS_MAIL_CLIENT_DATABASE_VERSION) {
                elog("Unable to open mail client: database is wrong version. Supported: ${s}, stored: ${v}",
                     ("s", BTS_MAIL_CLIENT_DATABASE_VERSION)("v", _property_db.fetch("version").as_int64()));
                FC_ASSERT(false, "Mail client database is an unknown version.");
            }

            //Place all in-processing messages back in their place on the pipeline
            for (auto itr = _processing_db.begin(); itr.valid(); ++itr)
                retry_message(itr.value());

            index_archive();
        } catch (...) {
            _archive.close();
            _processing_db.close();
            _property_db.close();
        }
    }
    bool is_open() {
        return _property_db.is_open();
    }

    void index_archive() {
        _archive_indexing_future = _archive_indexing_thread.async([this] {
            for (auto itr = _archive.begin(); itr.valid() && !_archive_indexing_future.canceled(); ++itr)
                _mail_index.insert(itr.value());
        }, "Mail client indexing task");
    }

    void process_outgoing_mail(mail_record& mail) {
        //Messages go through a pipeline of processing. This function starts them on that journey.
        mail.mail_servers = get_mail_servers_for_recipient(mail.recipient);
        _processing_db.store(mail.id, mail);

        //The steps required to send a message:
        //Get proof of work target from mail servers
        //Calculate proof of work
        //Send message to all applicable mail servers
        //Store message in the archive

        get_proof_of_work_target(mail.id);
    }

    unordered_set<string> get_mail_server_names_for_recipient(const string& recipient) {
        oaccount_record recipient_account = _chain->get_account_record(recipient);
        //If recipient account is not registered, just take default servers.
        if (!recipient_account)
            return BTS_MAIL_DEFAULT_MAIL_SERVERS;

        try {
            auto vector_servers = recipient_account->public_data.as<variant_object>()["mail_servers"]
                                                                .as<vector<string>>();
            unordered_set<string> servers;
            for (auto&& server : vector_servers)
                servers.insert(std::move(server));
            return servers;
        } catch (fc::exception& e) {
            elog("Error while getting mail servers for ${r}: ${e}", ("r", recipient)("e", e.to_detail_string()));
        }
        ilog("It appears that ${r} has not published his preferred mail servers. Using defaults.",
             ("r", recipient));
        return BTS_MAIL_DEFAULT_MAIL_SERVERS;

    }

    mail_server_list get_mail_servers_for_recipient(const string& recipient) {
        mail_server_list servers;
        try {
            auto server_list = get_mail_server_names_for_recipient(recipient);
            for (const string& server_name : server_list) {
                oaccount_record server_account = _chain->get_account_record(server_name);
                if (!server_account)
                    continue;
                auto endpoint = server_account->public_data.as<variant_object>()["mail_server_endpoint"]
                                                           .as<ip::endpoint>();
                servers.insert(make_pair(std::move(server_name), std::move(endpoint)));
            }
        } catch (fc::exception& e) {
            elog("Error while getting mail servers for ${r}: ${e}", ("r", recipient)("e", e.to_detail_string()));
        }

        return servers;
    }

    void get_proof_of_work_target(const message_id_type& message_id) {
        mail_record email = _processing_db.fetch(message_id);

        if (email.mail_servers.empty()) {
            email.status = client::failed;
            email.failure_reason = "Could not find mail servers for this recipient.";
            _processing_db.store(message_id, email);
            return;
        }

        //TODO: Contact mail servers, get their PoW requirements, set target to min() of these
        email.proof_of_work_target = BTS_MAIL_PROOF_OF_WORK_TARGET;
        _processing_db.store(message_id, email);

        schedule_proof_of_work(message_id);
    }

    template<typename TaskData>
    void schedule_generic_task(job_queue& queue,
                               fc::future<void>& future,
                               TaskData data,
                               std::function<void(TaskData)> task,
                               const char* task_description) {
        queue.push(data);

        if (future.valid() && !future.ready())
            return;

        future = fc::async([task, task_description, &queue]{
            while (!queue.empty()) {
                TaskData data = queue.front();
                queue.pop();
                task(data);
            }
        }, task_description);
    }

    void schedule_proof_of_work(const message_id_type& message_id) {
        schedule_generic_task<message_id_type>(_proof_of_work_jobs, _proof_of_work_worker, message_id,
                                               [this](message_id_type message_id){
            //Use a unique_ptr to ensure deletion, but a raw pointer to copy into the worker thread
            std::unique_ptr<mail_record> email_uptr(new mail_record(_processing_db.fetch(message_id)));
            mail_record* email = email_uptr.get();

            if (email->status != client::canceled && email->proof_of_work_target != ripemd160()) {
                email->status = client::proof_of_work;
                _processing_db.store(email->id, *email);
            } else {
                //Don't have a proof-of-work target or message canceled; cannot continue
                email->status = client::failed;
                email->failure_reason = (email->status == client::canceled?
                                             "Canceled by user." :
                                             "No proof of work target. Cannot do proof of work.");
                _processing_db.store(email->id, *email);
                return;
            }

            std::unique_ptr<fc::future<void>> slave_handle_uptr(new fc::future<void>());
            fc::future<void>* slave_handle = slave_handle_uptr.get();
            while (_processing_db.fetch(message_id).status != client::canceled &&
                   email->content.id() > email->proof_of_work_target) {
                email->content.timestamp = blockchain::now();
                _processing_db.store(email->id, *email);

                try {
                    *slave_handle = _proof_of_work_thread.async([email, slave_handle] {
                        fc::time_point start_time = fc::time_point::now();
                        while (!slave_handle->canceled() &&
                               fc::time_point::now() - start_time < fc::seconds(1) &&
                               email->content.id() > email->proof_of_work_target)
                            ++email->content.nonce;
                    }, "Mail client proof-of-work worker");

                    slave_handle->wait();
                } catch (fc::canceled_exception&) {
                    slave_handle->cancel();
                    _proof_of_work_thread.quit();
                    throw;
                }
            }

            if (_processing_db.fetch(message_id).status == client::canceled) {
                email->status = client::failed;
                email->failure_reason = "Canceled by user.";
                _processing_db.store(message_id, *email);
                return;
            }

            _processing_db.store(email->id, *email);
            schedule_transmit_message(email->id);
            fc::yield();
        }, "Mail client proof-of-work supervisor");
    }

    void schedule_transmit_message(message_id_type message_id) {
        schedule_generic_task<message_id_type>(_transmit_message_jobs, _transmit_message_worker, message_id,
                                               [this](message_id_type message_id){
            mail_record email = _processing_db.fetch(message_id);
            if (email.mail_servers.empty()) {
                email.status = client::failed;
                email.failure_reason = "No mail servers found when trying to transmit message.";
                _processing_db.store(message_id, email);
                return;
            } else {
                email.status = client::transmitting;
                _processing_db.store(message_id, email);
            }

            vector<fc::future<void>> transmit_tasks;
            transmit_tasks.reserve(email.mail_servers.size());
            mail_server_list successful_servers;

            for (mail_server_endpoint server : email.mail_servers) {
                transmit_tasks.push_back(fc::async([&, server] {
                    auto email = _processing_db.fetch(message_id);
                    tcp_socket sock;

                    try {
                        sock.connect_to(server.second);
                    } catch (fc::exception& e) {
                        if (successful_servers.empty()) {
                            //Mark as failed only if no servers have succeeded yet.
                            //If it later succeeds, the status will be updated accordingly.
                            email.failure_reason = e.what();
                            email.status = client::failed;
                            _processing_db.store(message_id, email);
                        }
                        return;
                    }

                    mutable_variant_object request;
                    request["id"] = 0;
                    request["method"] = "mail_store_message";
                    request["params"] = vector<variant>({variant(email.content)});

                    fc::json::to_stream(sock, variant_object(request));
                    string raw_response;
                    fc::getline(sock, raw_response);
                    variant_object response = fc::json::from_string(raw_response).as<variant_object>();

                    if (response["id"].as_int64() != 0)
                        wlog("Server response has wrong ID... attempting to press on. Expected: 0; got: ${r}",
                             ("r", response["id"]));
                    if (response.contains("error")) {
                        //Server actively rejects email. Something is definitely wrong; declare failure.
                        email.status = client::failed;
                        fc::exception except = response["error"].as<fc::exception>();
                        email.failure_reason = except.what();
                        if (email.failure_reason == message_already_stored().what()) {
                            //Message is stored! Don't store email; there's no error. We're done.
                            wlog("Message ${id} already stored on server ${server}.",
                                 ("id", message_id)("server", server));
                            successful_servers.insert(std::move(server));
                            return;
                        } else if (email.failure_reason == timestamp_too_old().what()) {
                            //Redo the proof-of-work
                            email.status = client::proof_of_work;
                            email.content.nonce++;
                        }
                        _processing_db.store(message_id, email);
                        elog("Storing message with server ${server} failed: ${error}",
                             ("server", server)("error", response["error"])("request", request));
                        sock.close();
                        return;
                    }

                    request["id"] = 1;
                    request["method"] = "mail_fetch_message";
                    request["params"] = vector<variant>({variant(email.content.id())});

                    fc::json::to_stream(sock, variant_object(request));
                    fc::getline(sock, raw_response);
                    response = fc::json::from_string(raw_response).as<variant_object>();

                    if (response["id"].as_int64() != 1)
                        wlog("Server response has wrong ID... attempting to press on. Expected: 1; got: ${r}",
                             ("r", response["id"]));
                    if (response["result"].as<message>().id() != email.content.id()) {
                        //This should only happen in case of ripemd160 collision, I think... Hopefully never.
                        email.status = client::failed;
                        email.failure_reason = "Message saved to server, but server responded with "
                                               "another message when we requested it.";
                        _processing_db.store(message_id, email);
                        elog("Storing message with server ${server} failed because server gave back wrong message.",
                             ("server", server));
                        sock.close();
                        return;
                    }

                    successful_servers.insert(std::move(server));
                }, "Mail client transmitter"));
            }

            auto timeout_future = fc::schedule([=] {
                auto email = _processing_db.fetch(message_id);
                //Timed out. If any servers succeeded, we'll take the win. If no server succeeded
                //and the email didn't already get pushed back in the pipeline, fail it.
                if (successful_servers.empty() && email.status >= client::transmitting) {
                   ulog("Email ${id}: Timeout when transmitting", ("id", email.id));
                   email.status = client::failed;
                   email.failure_reason = "Timed out while transmitting message.";
                   _processing_db.store(email.id, email);
                }
                for (auto task_future : transmit_tasks)
                    task_future.cancel();
            }, fc::time_point::now() + fc::seconds(10), "Mail client transmitter timeout");

            while (!transmit_tasks.empty()) {
                transmit_tasks.back().wait();
                email = _processing_db.fetch(message_id);
                if (email.status == client::failed) {
                    for (auto task_future : transmit_tasks)
                        task_future.cancel_and_wait();
                    return;
                }
                transmit_tasks.pop_back();
            }
            timeout_future.cancel("Finished transmitting");

            if (!successful_servers.empty()) {
                email.mail_servers = successful_servers;
                _processing_db.store(message_id, email);
                finalize_message(message_id);
            }
        }, "Mail client transmit message");
    }

    void finalize_message(message_id_type message_id) {
        mail_record email = _processing_db.fetch(message_id);
        ulog("Email ${id} sent successfully, and is now known as ${newid}.",
             ("id", message_id)("newid", email.content.id()));
        email.id = email.content.id();
        email.status = client::accepted;
        _mail_index.insert(email_header(email));
        _archive.store(email.id, std::move(email));
        _processing_db.remove(message_id);
    }

    template<typename Database>
    std::multimap<client::mail_status, message_id_type> get_database_messages(Database& db) {
        std::multimap<client::mail_status, message_id_type> messages;
        for(auto itr = db.begin(); itr.valid(); ++itr) {
            auto email = itr.value();
            messages.insert(std::make_pair(email.status, email.id));
        }
        return messages;
    }

    email_record decrypted_email_record(mail_record&& email) {
        if (email.content.type != encrypted)
            return email;
        email.content = _wallet->mail_open(email.recipient_key, email.content);
        return email;
    }
    email_record decrypted_email_record(mail_archive_record&& email) {
        if (email.content.type != encrypted)
            return email;
        email.content = _wallet->mail_open(email.recipient_address, email.content);
        return email;
    }

    email_record get_message(message_id_type message_id) {
        if (auto op = _processing_db.fetch_optional(message_id))
            return decrypted_email_record(std::move(*op));
        if (auto op = _archive.fetch_optional(message_id))
            return decrypted_email_record(std::move(*op));
        FC_ASSERT(false, "Message ${id} not found.", ("id", message_id));
    }

    vector<email_header> get_messages_by_sender(string sender) {
        vector<email_header> results;

        for (auto itr = _mail_index.get<by_sender>().lower_bound(boost::make_tuple(sender));
             itr != _mail_index.get<by_sender>().upper_bound(boost::make_tuple(sender));
             ++itr)
            results.push_back(get_message(itr->id).header);

        return results;
    }

    vector<email_header> get_messages_by_recipient(string recipient) {
        vector<email_header> results;

        for (auto itr = _mail_index.get<by_recipient>().lower_bound(boost::make_tuple(recipient));
             itr != _mail_index.get<by_recipient>().upper_bound(boost::make_tuple(recipient));
             ++itr)
            results.push_back(get_message(itr->id).header);

        return results;
    }

    vector<email_header> get_messages_from_to(string sender, string recipient) {
        vector<email_header> results;

        for (auto itr = _mail_index.get<by_sender>().lower_bound(boost::make_tuple(sender, recipient));
             itr != _mail_index.get<by_sender>().upper_bound(boost::make_tuple(sender, recipient));
             ++itr)
            results.push_back(get_message(itr->id).header);

        return results;
    }

    vector<email_header> get_inbox() {
        vector<email_header> inbox;
        for (auto itr = _inbox.begin(); itr.valid(); ++itr)
            inbox.push_back(itr.value());
        std::sort(inbox.begin(), inbox.end(), [](const email_header& a, const email_header& b) {
            return a.timestamp < b.timestamp;
        });
        return inbox;
    }

    void archive_message(message_id_type message_id) {
        if (_inbox.fetch_optional(message_id))
            _inbox.remove(message_id);
    }

    int check_new_mail(bool get_old_messages) {
        auto accounts = _wallet->list_my_accounts();
        _messages_in = 0;

        for (wallet_account_record account : accounts) {
            auto servers = get_mail_servers_for_recipient(account.name);
            vector<fc::future<void>> fetch_tasks;
            fetch_tasks.reserve(servers.size());

            auto last_check_time = account.registration_date;
            fc::time_point_sec check_time = _chain->now();
            fc::optional<variant> op;
            if (!get_old_messages && (op = _property_db.fetch_optional("last_fetch/" + account.name)))
                last_check_time = op->as<fc::time_point_sec>();

            for (mail_server_endpoint server : servers) {
                fetch_tasks.push_back(fc::async([=] {
                    //TODO: This whole design needs to be rethought. This is just a simplistic first effort.
                    //Right now we get the inventory, then download and store ALL of it locally.
                    //Downloading is done synchronously, with one message downloaded before the next starts.
                    //No deduplication of effort is done; i.e. if a given message is on three servers, we'll download
                    //it three times.
                    tcp_socket sock;

                    try {
                        sock.connect_to(server.second);
                    } catch (fc::exception& e) {
                        elog("Failed to connect to mail server ${server}: ${e}",
                             ("server", server)("e", e.to_detail_string()));
                        return;
                    }

                    int received = BTS_MAIL_CLIENT_MAX_INVENTORY_SIZE;
                    while (received == BTS_MAIL_CLIENT_MAX_INVENTORY_SIZE) {
                        mutable_variant_object request;
                        request["id"] = 0;
                        request["method"] = "mail_fetch_inventory";
                        request["params"] = vector<variant>({variant(address(account.account_address)),
                                                             variant(last_check_time),
                                                             variant(BTS_MAIL_CLIENT_MAX_INVENTORY_SIZE)});

                        fc::json::to_stream(sock, variant_object(request));
                        string raw_response;
                        fc::getline(sock, raw_response);
                        variant_object response = fc::json::from_string(raw_response).as<variant_object>();

                        if (response["id"].as_int64() != 0)
                            wlog("Server response has wrong ID... attempting to press on. Expected: 0; got: ${r}",
                                 ("r", response["id"]));
                        if (response.contains("error")) {
                            elog("Server ${server} gave error ${error} on request ${request}",
                                 ("server", server)("error", response["error"])("request", request));
                            sock.close();
                            return;
                        }

                        inventory_type results = response["result"].as<inventory_type>();
                        received = results.size();

                        for (std::pair<fc::time_point, message_id_type> email : results) {
                            request["id"] = 1;
                            request["method"] = "mail_fetch_message";
                            request["params"] = vector<variant>({variant(email.second)});

                            fc::json::to_stream(sock, variant_object(request));
                            fc::getline(sock, raw_response);
                            response = fc::json::from_string(raw_response).as<variant_object>();

                            if (response["id"].as_int64() != 1)
                                wlog("Server response has wrong ID... attempting to press on. Expected: 1; got: ${r}",
                                     ("r", response["id"]));
                            if (response.contains("error")) {
                                elog("Server ${server} gave error ${error} on request ${request}",
                                     ("server", server)("error", response["error"])("request", request));
                                sock.close();
                                return;
                            }

                            message ciphertext = response["result"].as<message>();
                            message plaintext = _wallet->mail_open(account.account_address, ciphertext);
                            email_header header;
                            header.id = ciphertext.id();
                            if (plaintext.type == mail::email) {
                                signed_email_message email = plaintext.as<signed_email_message>();
                                try {
                                   header.sender = _wallet->get_key_label(email.from());
                                } catch (fc::exception& e) {
                                   header.sender = "INVALID SIGNATURE";
                                }
                                header.subject = std::move(email.subject);
                            } else if (plaintext.type == mail::transaction_notice) {
                                transaction_notice_message notice = plaintext.as<transaction_notice_message>();
                                try {
                                   header.sender = _wallet->get_key_label(notice.from());
                                } catch (fc::exception& e) {
                                   header.sender = "INVALID SIGNATURE";
                                }
                                header.subject = "Transaction Notification";
                                _wallet->scan_transaction(notice.trx.id().str(), true);
                                self->new_transaction_notifier(notice);
                            }
                            header.recipient = account.name;
                            header.timestamp = plaintext.timestamp;
                            mail_archive_record record(std::move(ciphertext), header, account.account_address);
                            bool new_mail = false;

                            if (auto optional_record = _archive.fetch_optional(email.second)) {
                                record = *optional_record;
                                if (record.status == client::accepted) {
                                    //We sent this message, but it's still newly received mail
                                    new_mail = true;
                                    record.status = client::received;
                                }
                            } else
                                new_mail = true;

                            record.mail_servers.insert(std::move(server));

                            _archive.store(email.second, record);
                            _mail_index.insert(header);

                            if (new_mail) {
                                _inbox.store(header.id, header);
                                ++_messages_in;
                            }
                        }
                    }
                }, "Mail client fetcher"));
            }

            auto timeout_future = fc::schedule([=] {
                elog("Timed out fetching new mail.");
                ulog("Timed out fetching new mail.");
                for (auto task_future : fetch_tasks)
                    task_future.cancel();
            }, fc::time_point::now() + fc::seconds(60), "Mail client fetcher timeout");

            while (!fetch_tasks.empty()) {
                fetch_tasks.back().wait();
                fetch_tasks.pop_back();
            }

            timeout_future.cancel("Finished fetching");
            _property_db.store("last_fetch/" + account.name, variant(check_time));
        }

        return _messages_in;
    }
};

}

client::client(wallet_ptr wallet, chain_database_ptr chain)
    : my(new detail::client_impl(this, wallet, chain))
{
}

void client::open(const path& data_dir) {
    my->open(data_dir);
}

void client::retry_message(message_id_type message_id)
{
    FC_ASSERT(my->is_open());
    auto itr = my->_processing_db.find(message_id);
    FC_ASSERT(itr.valid(), "Message not found.");
    auto email = itr.value();
    FC_ASSERT(email.status == failed, "Message has not failed to send; cannot retry sending.");
    email.status = submitted;
    my->retry_message(email);
}

void client::cancel_message(message_id_type message_id)
{
    FC_ASSERT(my->is_open());
    auto itr = my->_processing_db.find(message_id);
    if (itr.valid()) {
        FC_ASSERT(itr.value().status <= proof_of_work, "Cannot cancel message once it has been submitted to servers.");
        detail::mail_record cancel_mail = itr.value();
        cancel_mail.status = canceled;
        my->_processing_db.store(message_id, cancel_mail);
    }
}

void client::remove_message(message_id_type message_id)
{
    FC_ASSERT(my->is_open());
    auto itr = my->_processing_db.find(message_id);
    if (itr.valid()) {
        FC_ASSERT(itr.value().status == failed, "Cannot remove message during processing.");
        my->_processing_db.remove(message_id);
    } else {
        auto itr = my->_archive.find(message_id);
        if (itr.valid())
            my->_archive.remove(message_id);
    }
}

void client::archive_message(message_id_type message_id_type)
{
    FC_ASSERT(my->is_open());
    my->archive_message(message_id_type);
}

int client::check_new_messages(bool get_old_messages)
{
    FC_ASSERT(my->is_open());
    int new_messages = my->check_new_mail(get_old_messages);
    if (new_messages > 0)
        new_mail_notifier(new_messages);
    return new_messages;
}

std::multimap<client::mail_status, message_id_type> client::get_processing_messages() {
    FC_ASSERT(my->is_open());
    return my->get_database_messages(my->_processing_db);
}

std::multimap<client::mail_status, message_id_type> client::get_archive_messages()
{
    FC_ASSERT(my->is_open());
    return my->get_database_messages(my->_archive);
}

std::vector<email_header> client::get_inbox()
{
    FC_ASSERT(my->is_open());
    return my->get_inbox();
}

email_record client::get_message(message_id_type message_id) {
    FC_ASSERT(my->is_open());
    return my->get_message(message_id);
}

message_id_type client::send_email(const string &from,
                                   const string &to,
                                   const string &subject,
                                   const string &body,
                                   const message_id_type& reply_to) {
    FC_ASSERT(my->_wallet->is_open());
    FC_ASSERT(my->_wallet->is_unlocked());
    FC_ASSERT(my->is_open());

    //TODO: Find a thin-clienty way to do this, rather than calling a local chain_database
    oaccount_record recipient = my->_chain->get_account_record(to);
    FC_ASSERT(recipient, "Could not find recipient account: ${name}", ("name", to));

    //All mail shall be addressed to the owner key, but encrypted with the active key.
    message plaintext = my->_wallet->mail_create(from, subject, body, reply_to);
    plaintext.recipient = recipient->owner_key;
    detail::mail_record email(from, to, recipient->owner_key,
                              my->_wallet->mail_encrypt(recipient->active_key(), plaintext));
    email.content.recipient = plaintext.recipient;
    my->process_outgoing_mail(email);

    return email.id;
}

message_id_type client::send_encrypted_message(message&& ciphertext,
                                               const string& from,
                                               const string& to,
                                               const public_key_type& recipient_key)
{
    FC_ASSERT(my->is_open());
    FC_ASSERT(ciphertext.type == encrypted, "Refusing to send plaintext message");

    ciphertext.recipient = recipient_key;
    detail::mail_record mail_rec(from, to, recipient_key, std::move(ciphertext));
    my->process_outgoing_mail(mail_rec);

    return mail_rec.id;
}

std::vector<email_header> client::get_messages_by_sender(std::string sender)
{
    FC_ASSERT(my->is_open());
    if (my->_archive_indexing_future.valid() && !my->_archive_indexing_future.ready()) {
        ulog("Mail archive is currently indexing. Please try again later.");
        return vector<email_header>();
    }

    return my->get_messages_by_sender(sender);
}

std::vector<email_header> client::get_messages_by_recipient(std::string recipient)
{
    FC_ASSERT(my->is_open());
    if (my->_archive_indexing_future.valid() && !my->_archive_indexing_future.ready()) {
        ulog("Mail archive is currently indexing. Please try again later.");
        return vector<email_header>();
    }

    return my->get_messages_by_recipient(recipient);
}

std::vector<email_header> client::get_messages_from_to(std::string sender, std::string recipient)
{
    FC_ASSERT(my->is_open());
    if (my->_archive_indexing_future.valid() && !my->_archive_indexing_future.ready()) {
        ulog("Mail archive is currently indexing. Please try again later.");
        return vector<email_header>();
    }

    return my->get_messages_from_to(sender, recipient);
}

email_header::email_header(const detail::mail_record &processing_record)
    : id(processing_record.id),
      sender(processing_record.sender),
      recipient(processing_record.recipient),
      timestamp(processing_record.content.timestamp)
{
    if (processing_record.content.type == email)
    {
        auto sem = processing_record.content.as<signed_email_message>();
        subject = sem.subject;
    }
}

email_header::email_header(const detail::mail_archive_record& archive_record)
    : id(archive_record.id),
      sender(archive_record.sender),
      recipient(archive_record.recipient),
      timestamp(archive_record.content.timestamp)
{
    if (archive_record.content.type == email)
    {
        auto sem = archive_record.content.as<signed_email_message>();
        subject = sem.subject;
    }
}

email_record::email_record(const detail::mail_record& processing_record)
    : header(processing_record),
      content(processing_record.content),
      mail_servers(processing_record.mail_servers)
{
    header.id = processing_record.id;
    header.sender = processing_record.sender;
    header.recipient = processing_record.recipient;
    if (processing_record.status == client::failed)
        failure_reason = processing_record.failure_reason;
}
email_record::email_record(const detail::mail_archive_record& archive_record)
    : header(archive_record),
      content(archive_record.content),
      mail_servers(archive_record.mail_servers)
{}

}
}

FC_REFLECT(bts::mail::detail::mail_record, (id)(status)(sender)(recipient)
           (recipient_key)(content)(mail_servers)(proof_of_work_target))
FC_REFLECT(bts::mail::detail::mail_archive_record, (id)(status)(sender)
           (recipient)(recipient_address)(content)(mail_servers))
