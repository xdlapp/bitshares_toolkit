#include <bts/wallet/wallet.hpp>
#include <bts/wallet/wallet_db.hpp>
#include <bts/wallet/config.hpp>
#include <bts/blockchain/time.hpp>
#include <fc/thread/thread.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/filesystem.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>

#include <fc/io/json.hpp>
#include <iostream>

#include <algorithm>

namespace bts { namespace wallet {

   namespace detail {

      class wallet_impl : public chain_observer
      {
         public:
             wallet*            self;
             wallet_db          _wallet_db;
             chain_database_ptr _blockchain;
             path               _data_directory;
             path               _current_wallet_path;
             fc::time_point     _scheduled_lock_time;
             fc::future<void>   _wallet_relocker_done;
             fc::sha512         _wallet_password;

            /**
             * This method is called anytime the blockchain state changes including
             * undo operations.
             */
            void state_changed( const pending_chain_state_ptr& state )
            {
            }
            /**
             *  This method is called anytime a block is applied to the chain.
             */
            void block_applied( const block_summary& summary ) override
            {
               if( self->is_unlocked() )
               {
                  auto account_priv_keys = _wallet_db.get_account_private_keys( _wallet_password );
                  scan_block( summary.block_data.block_num, account_priv_keys );
               }
            }

             secret_hash_type get_secret( uint32_t block_num,
                                          const private_key_type& delegate_key )const;

             void scan_block( uint32_t block_num, 
                              const vector<private_key_type>& keys );

             bool scan_withdraw( const withdraw_operation& op );

             bool scan_deposit( wallet_transaction_record& trx_rec, const deposit_operation& op, 
                                 const private_keys& keys );

             bool scan_register_account( const register_account_operation& op );
             bool scan_update_account( const update_account_operation& op );
             bool scan_create_asset( wallet_transaction_record& trx_rec, const create_asset_operation& op );
             bool scan_issue_asset( wallet_transaction_record& trx_rec, const issue_asset_operation& op );

             bool cache_balance( const balance_id_type& balance_id );

             void clear_pending_transactions();

             void scan_balances();
             void scan_registered_accounts();
             void withdraw_to_transaction( share_type amount,
                                           asset_id_type asset_id,
                                           const address& from_account_address,
                                           signed_transaction& trx, 
                                           unordered_set<address>& required_fees );
             bool address_in_account( const address& address_to_check,
                                      const address& account_address )const;

      };

      void wallet_impl::clear_pending_transactions()
      {
          _wallet_db.clear_pending_transactions();
      }
      void wallet_impl::scan_balances()
      {
         _blockchain->scan_balances( [=]( const balance_record& bal_rec )
         {
              auto key_rec =_wallet_db.lookup_key( bal_rec.owner() );
              if( key_rec.valid() && key_rec->has_private_key() )
              {
                ilog( "     found balance ${id}  ${amount}", ("id",bal_rec.id())("amount", _blockchain->to_pretty_asset(bal_rec.get_balance()) ) );
                _wallet_db.cache_balance( bal_rec );
              }
         } );
      }
      void wallet_impl::scan_registered_accounts()
      {
         _blockchain->scan_accounts( [=]( const blockchain::account_record& account_rec )
         {
              auto key_rec =_wallet_db.lookup_key( account_rec.active_key() );
              if( key_rec.valid() && key_rec->has_private_key() )
              {
                 auto a = _wallet_db.lookup_account( key_rec->account_address );
                 if( a.valid() )
                 {
                    account_record& ar = *a;
                    ar = account_rec;
                    _wallet_db.store_record( *a );
                 }
              }
         } );
      }

      bool wallet_impl::address_in_account( const address& address_to_check, 
                                            const address& account_address )const
      { try {
         auto opt_key = _wallet_db.lookup_key( address_to_check );
         if( !opt_key.valid() )
            return false;
         return opt_key->account_address == account_address;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("address_to_check",address_to_check)("account_address",account_address) ) }

      void wallet_impl::withdraw_to_transaction( share_type amount,
                                                 asset_id_type asset_id,
                                                 const address& from_account_address,
                                                 signed_transaction& trx, 
                                                 unordered_set<address>& required_signatures )
      {
         share_type remaining = amount;
         for( auto& balance_item : _wallet_db.balances )
         {
            auto owner = balance_item.second.owner();
            if( balance_item.second.asset_id() == asset_id && 
                address_in_account( owner, from_account_address ) )
            {
               if( balance_item.second.balance > 0 )
               {
                  if( remaining > balance_item.second.balance )
                  {
                     trx.withdraw( balance_item.first, balance_item.second.balance );
                     balance_item.second.balance = 0;
                     remaining -= balance_item.second.balance;
                     required_signatures.insert( balance_item.second.owner() );
                     _wallet_db.store_record( balance_item.second );
                  }
                  else
                  {
                     trx.withdraw( balance_item.first, remaining );
                     balance_item.second.balance -= remaining;
                     remaining = 0;
                     _wallet_db.store_record( balance_item.second );
                     required_signatures.insert( balance_item.second.owner() );
                     return;
                  }
              }
            }
         }
         FC_ASSERT( !"Insufficient Funds", "Requested ${required} but only ${available} available",
                    ("required", _blockchain->to_pretty_asset( asset(amount,asset_id) ) )
                    ("available", _blockchain->to_pretty_asset( asset(amount-remaining,asset_id) ) )
                  );
      }


      secret_hash_type wallet_impl::get_secret( uint32_t block_num, 
                                                const private_key_type& delegate_key )const
      {
         block_id_type header_id;
         if( block_num != uint32_t(-1) && block_num > 1 )
         {
            auto block_header = _blockchain->get_block_header( block_num - 1 );
            header_id = block_header.id();
         }

         fc::sha512::encoder key_enc;
         fc::raw::pack( key_enc, delegate_key );
         fc::sha512::encoder enc;
         fc::raw::pack( enc, key_enc.result() );
         fc::raw::pack( enc, header_id );

         return fc::ripemd160::hash( enc.result() );
      }

      void wallet_impl::scan_block( uint32_t block_num, 
                                    const private_keys& keys )
      {
         //std::cout << "scanning block number " << block_num << "    keys: " << keys.size() << "    \r";
         auto current_block = _blockchain->get_block( block_num );
         for( auto trx : current_block.user_transactions )
         {
            bool cache_trx = false;
            //std::cout << "scanning block number " << block_num << "    \n";
            //std::cout << "    scanning trx: " << fc::json::to_string( trx) << "    \n";
            owallet_transaction_record current_trx_record = _wallet_db.lookup_transaction( trx.id() );
            if( current_trx_record.valid() )
            {
               cache_trx = true;
            }
            else
            {
               current_trx_record = wallet_transaction_record();
            }
            current_trx_record->block_num = block_num;
            current_trx_record->trx = trx;
            current_trx_record->received_time = current_block.timestamp;

            for( auto op : trx.operations )
            {
               switch( (operation_type_enum)op.type )
               {
                  case withdraw_op_type:
                     cache_trx |= scan_withdraw( op.as<withdraw_operation>() );
                     break;
                  case deposit_op_type:
                     cache_trx |= scan_deposit( *current_trx_record, op.as<deposit_operation>(), keys );
                     break;
                  case register_account_op_type:
                     cache_trx |= scan_register_account( op.as<register_account_operation>() );
                     break;
                  case update_account_op_type:
                     cache_trx |= scan_update_account( op.as<update_account_operation>() );
                     break;
                   case create_asset_op_type:
                     cache_trx |= scan_create_asset( *current_trx_record, op.as<create_asset_operation>() );
                     break;
                   case issue_asset_op_type:
                     cache_trx |= scan_issue_asset( *current_trx_record, op.as<issue_asset_operation>() );
                     break;
               }
            }
            if( cache_trx )
               _wallet_db.store_transaction( *current_trx_record );
         }
      }
      bool wallet_impl::scan_withdraw( const withdraw_operation& op )
      {
         auto current_balance = _wallet_db.lookup_balance( op.balance_id );
         if( current_balance.valid() )
         {
            cache_balance( op.balance_id );
         }
         return current_balance.valid();
      }

      bool wallet_impl::scan_register_account( const register_account_operation& op )
      {
          auto opt_key_rec = _wallet_db.lookup_key( op.owner_key );

          if( !opt_key_rec.valid() ) 
             return false;

          auto opt_account = _wallet_db.lookup_account( address( op.owner_key ) );
          if( !opt_account.valid() )
          {
             wlog( "We have the key but no account for registration operation" );
             return false;
          }

          wlog( "we detected an account register operation for ${name}", ("name",op.name) );
          auto account_name_rec = _blockchain->get_account_record( op.name );
          FC_ASSERT( account_name_rec.valid() );

          blockchain::account_record& tmp  = *opt_account;
          tmp = *account_name_rec; //->id = account_name_rec->id;
          _wallet_db.account_id_to_account[account_name_rec->id] = opt_account->index;
          _wallet_db.store_record( *opt_account );
          _wallet_db.accounts[ opt_account->index ] = *opt_account;

          return false;
      }




      bool wallet_impl::scan_update_account( const update_account_operation& op )
      { try {
          auto oaccount =  _blockchain->get_account_record( op.account_id ); 
          FC_ASSERT( oaccount.valid() );
          auto opt_key_rec = _wallet_db.lookup_key( oaccount->owner_key );
          if( !opt_key_rec.valid() ) 
             return false;

          auto opt_account = _wallet_db.lookup_account( address( oaccount->owner_key ) );
          if( !opt_account.valid() )
          {
             wlog( "We have the key but no account for registration operation" );
             return false;
          }
          wlog( "we detected an account register operation for ${name}", ("name",oaccount->name) );
          auto account_name_rec = _blockchain->get_account_record( oaccount->name );
          FC_ASSERT( account_name_rec.valid() );

          blockchain::account_record& tmp  = *opt_account;
          tmp = *account_name_rec; //->id = account_name_rec->id;
          _wallet_db.account_id_to_account[account_name_rec->id] = opt_account->index;
          _wallet_db.store_record( *opt_account );
          _wallet_db.accounts[ opt_account->index ] = *opt_account;

          return false;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("op",op) ) }

      bool wallet_impl::scan_create_asset( wallet_transaction_record& trx_rec, const create_asset_operation& op  )
      {
         wlog( "${op}", ("op",op) );
         auto oissuer =  _blockchain->get_account_record( op.issuer_account_id );
         FC_ASSERT( oissuer.valid() );
         auto opt_key_rec = _wallet_db.lookup_key( oissuer->owner_key );
         if( opt_key_rec.valid() && opt_key_rec->has_private_key() )
         {
            trx_rec.to_account = oissuer->owner_key;
            trx_rec.from_account = oissuer->owner_key;
            trx_rec.memo_message = "create " + op.symbol + " ("+op.name+")";
         }
         return true;
      }
      bool wallet_impl::scan_issue_asset( wallet_transaction_record& trx_rec, const issue_asset_operation& op  )
      {
         wlog( "${op}", ("op",op) );
         /*
         auto oissuer =  _blockchain->get_account_record( op.issuer_account_id );
         FC_ASSERT( oissuer.valid() );
         auto opt_key_rec = _wallet_db.lookup_key( oissuer->owner_key );
         if( opt_key_rec.valid() && opt_key_rec->has_private_key() )
         {
            trx_rec.to_account = oissuer->owner_key;
            trx_rec.from_account = oissuer->owner_key;
            trx_rec.memo_message = "create " + op.symbol + " ("+op.name+")";
         }
         */
         return false;
      }

      bool wallet_impl::scan_deposit( wallet_transaction_record& trx_rec, 
                                      const deposit_operation& op, 
                                      const private_keys& keys )
      { try {
          bool cache_deposit = false; 
          switch( (withdraw_condition_types) op.condition.type )
          {
             case withdraw_signature_type:
                cache_deposit |= _wallet_db.has_private_key( op.condition.as<withdraw_with_signature>().owner );
                break;
             case withdraw_by_account_type:
             {
                for( auto key : keys )
                {
                   auto deposit = op.condition.as<withdraw_by_account>();
                   omemo_status status = deposit.decrypt_memo_data( key );
                   if( status.valid() )
                   {
                      _wallet_db.cache_memo( *status, key, _wallet_password );
                      if( status->memo_flags == from_memo )
                      {
                         trx_rec.memo_message = status->get_message();
                         trx_rec.amount       = asset( op.amount, op.condition.asset_id );
                         trx_rec.from_account = status->from;
                         trx_rec.to_account   = key.get_public_key();
                         //ilog( "FROM MEMO... ${msg}", ("msg",trx_rec.memo_message) );
                      }
                      else
                      {
                         //ilog( "TO MEMO OLD STATE: ${s}",("s",trx_rec) );
                         //ilog( "op: ${op}", ("op",op) );
                         trx_rec.memo_message = status->get_message();
                         trx_rec.from_account = key.get_public_key();
                         trx_rec.to_account   = status->from;
                         //ilog( "TO MEMO NEW STATE: ${s}",("s",trx_rec) );
                      }
                      cache_deposit = true;
                      break;
                   }
                }
                break;
             }
             // TODO: support other withdraw types here..
          }
          if( cache_deposit )
          {
             if( !cache_balance( op.balance_id() ) )
             {
                elog( "unable to cache balance ${b}", ("b",op) );
             }
          }
          return cache_deposit;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("op",op) ) } // wallet_impl::scan_deposit 

      bool wallet_impl::cache_balance( const balance_id_type& balance_id )
      {
         auto bal_rec = _blockchain->get_balance_record( balance_id );
         if( !bal_rec.valid() )
         {
            // wlog( "blockchain doesn't know about balance id: ${balance_id}",
            //      ("balance_id",balance_id) );
            _wallet_db.balances.erase( balance_id );
            return false;
         }
         else
         {
            _wallet_db.cache_balance( *bal_rec );
            return true;
         }
      }


   } // detail 

   wallet::wallet( chain_database_ptr blockchain )
   :my( new detail::wallet_impl() )
   {
      my->self = this;
      my->_blockchain = blockchain;
      my->_blockchain->set_observer( my.get() );
   }

   wallet::~wallet()
   {
      close();
   }

   void           wallet::set_data_directory( const path& data_dir )
   {
      my->_data_directory = data_dir;
   }

   path       wallet::get_data_directory()const
   {
      return my->_data_directory;
   }

   void wallet::create( const string& wallet_name, 
                        const string& password,
                        const string& brainkey  )
   { try {
      if( fc::exists( get_data_directory() / wallet_name ) )
      {
          std::cerr << "Wallet \"" << wallet_name << "\" already exists!\n";
          FC_THROW_EXCEPTION(invalid_arg_exception, "wallet name already exists", ("wal",wallet_name));
      }
      if (is_open())
        close();
      create_file( fc::absolute(my->_data_directory) / wallet_name, password, brainkey ); 
      open( wallet_name );
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${wallet_name}' in ${data_dir}", 
                            ("wallet_name",wallet_name)("data_dir",fc::absolute(my->_data_directory)) ) }


   void wallet::create_file( const path& wallet_file_path,
                        const string& password,
                        const string& brainkey  )
   { try {
      FC_ASSERT( !fc::exists( wallet_file_path ) );
      FC_ASSERT( password.size() > 8, "password size: ${size}", ("size",password.size()) );
      my->_wallet_db.open( wallet_file_path );
      
      my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );

      master_key new_master_key;
      if( brainkey.size() )
      {
         auto base = fc::sha512::hash( brainkey.c_str(), brainkey.size() );

         /* strengthen the key a bit */
         for( uint32_t i = 0; i < 100ll*1000ll; ++i )
            base = fc::sha512::hash( base );

         new_master_key.encrypt_key( my->_wallet_password, extended_private_key( base ) );
      }
      else
      {
         extended_private_key epk( private_key_type::generate() );
         new_master_key.encrypt_key( my->_wallet_password, epk );
      }
    
      my->_wallet_db.store_record( wallet_master_key_record( new_master_key,  -1 ) );

      my->_wallet_db.close();
      my->_wallet_db.open( wallet_file_path );

      FC_ASSERT( my->_wallet_db.wallet_master_key.valid() );

   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${wallet_file_path}'", 
                            ("wallet_file_path",wallet_file_path) ) }


   void wallet::open( const string& wallet_name )
   { try {
      open_file( get_data_directory() / wallet_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("wallet_name",wallet_name) ) }
   

   void wallet::open_file( const path& wallet_filename )
   { try {
      close();
      FC_ASSERT( fc::exists( wallet_filename ) );
      my->_wallet_db.open( wallet_filename );
      my->_current_wallet_path = wallet_filename;

      auto tmp_balances = my->_wallet_db.balances;
      for( auto item : tmp_balances )
         my->cache_balance( item.first );

   } FC_RETHROW_EXCEPTIONS( warn, "Unable to open wallet ${filename}", 
                                   ("filename",wallet_filename) ) }

   void wallet::close()
   { try {
      my->_wallet_db.close();
      if( my->_wallet_relocker_done.valid() )
      {
         lock();
         my->_wallet_relocker_done.cancel();
         if( my->_wallet_relocker_done.ready() ) 
           my->_wallet_relocker_done.wait();
      }
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   string wallet::get_wallet_name()const
   {
      return my->_current_wallet_path.filename().generic_string();
   }

   path wallet::get_wallet_filename()const
   {
      return my->_current_wallet_path;
   }

   bool wallet::is_open()const
   {
      return my->_wallet_db.is_open();
   }

   void wallet::export_to_json( const path& export_file_name ) const
   { try {
      my->_wallet_db.export_to_json( export_file_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("export_file_name",export_file_name) ) }

   void  wallet::create_from_json(const path& path, const string& name )
   { try {
      FC_ASSERT( !"Not Implemented" );

   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::unlock( const string& password, microseconds timeout )
   { try {
      FC_ASSERT( password.size() >= BTS_MIN_PASSWORD_LENGTH ) 
      FC_ASSERT( timeout >= fc::seconds(1) );
      FC_ASSERT( my->_wallet_db.wallet_master_key.valid() );

      my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );
      if( !my->_wallet_db.wallet_master_key->validate_password( my->_wallet_password ) )
      {
         lock();
         FC_ASSERT( !"Invalid Password" );
      }

      if( timeout == microseconds::maximum() )
      {
         my->_scheduled_lock_time = fc::time_point::maximum();
      }
      else
      {
         my->_scheduled_lock_time = fc::time_point::now() + timeout;
         if( !my->_wallet_relocker_done.valid() || my->_wallet_relocker_done.ready() )
         {
           my->_wallet_relocker_done = fc::async([this](){
             while( !my->_wallet_relocker_done.canceled() )
             {
               if (fc::time_point::now() > my->_scheduled_lock_time)
               {
                 lock();
                 return;
               }
               fc::usleep(microseconds(200000));
             }
           });
         }
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("timeout_sec", timeout.count()/1000000 ) ) }

   void wallet::lock()
   {
      my->_wallet_password     = fc::sha512();
      my->_scheduled_lock_time = fc::time_point();
      my->_wallet_relocker_done.cancel();
   }
   void wallet::change_passphrase( const string& new_passphrase )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( !"TODO - Implement CHange Passphrase" );
   }

   bool wallet::is_unlocked()const
   {
      return !wallet::is_locked();
   }   

   bool wallet::is_locked()const
   {
      return my->_wallet_password == fc::sha512();
   }

   fc::time_point wallet::unlocked_until()const
   {
      return my->_scheduled_lock_time;
   }

   public_key_type  wallet::create_account( const string& account_name, 
                                            const variant& private_data )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_name ) );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( !current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password );
      auto new_pub_key  = new_priv_key.get_public_key();

      my->_wallet_db.add_contact_account( account_name, new_pub_key, private_data );

      return new_pub_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   /**
    *  Creates a new private key under the specified account. This key
    *  will not be valid for sending TITAN transactions to, but will
    *  be able to receive payments directly.
    */
   address  wallet::get_new_address( const string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_name ) );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( !current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password, 
                                                          current_account->account_address );
      return new_priv_key.get_public_key();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   /**
    *  A contact is an account for which this wallet does not have the private
    *  key.   If account_name is globally registered then this call will assume
    *  it is the same account and will fail if the key is not the same.
    *
    *  @param account_name - the name the account is known by to this wallet
    *  @param key - the public key that will be used for sending TITAN transactions
    *               to the account.
    */
   void  wallet::add_contact_account( const string& account_name, 
                                      const public_key_type& key,
                                      const variant& private_data )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( account_name ) );

      auto current_registered_account = my->_blockchain->get_account_record( account_name );

      if( current_registered_account.valid() && current_registered_account->active_key() != key )
         FC_ASSERT( !"Account name is already registered under a different key" );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      if( current_account.valid() )
      {
         wlog( "current account is valid... ${name}", ("name", *current_account) );
         FC_ASSERT( current_account->account_address == address(key),
                    "Account with ${name} already exists", ("name",account_name) );
         current_account->private_data = private_data;
         my->_wallet_db.store_record( *current_account );
         return;
      }
      else
      {
         auto account_key = my->_wallet_db.lookup_key( address(key) );
         FC_ASSERT( !account_key.valid() );
         if( current_registered_account.valid() )
            my->_wallet_db.add_contact_account( *current_registered_account, private_data );
         else
            my->_wallet_db.add_contact_account( account_name, key, private_data );
         account_key = my->_wallet_db.lookup_key( address(key) );
         FC_ASSERT( account_key.valid() );
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name)("public_key",key) ) }


   owallet_account_record    wallet::get_account( const string& account_name )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( account_name ) );
      return my->_wallet_db.lookup_account( account_name );
   }

   void  wallet::remove_contact_account( const string& account_name )
   { try {
      auto oaccount = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( oaccount.valid() );
      FC_ASSERT( ! my->_wallet_db.has_private_key(address(oaccount->owner_key)),
              "you can only remove contact accounts" );
      my->_wallet_db.remove_contact_account( account_name );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name", account_name) ) }

   void  wallet::rename_account( const string& old_account_name, 
                                 const string& new_account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( old_account_name ) );
      FC_ASSERT( is_valid_account_name( new_account_name ) );

      auto old_account = my->_wallet_db.lookup_account( old_account_name );
      FC_ASSERT( old_account.valid() );

      auto new_account = my->_wallet_db.lookup_account( new_account_name );
      FC_ASSERT( !new_account.valid() );

      my->_wallet_db.rename_account( old_account_name, new_account_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", 
                ("old_account_name",old_account_name)
                ("new_account_name",new_account_name) ) }


   /**
    *  If we already have a key record for key, then set the private key.
    *  If we do not have a key record, 
    *     If account_name is a valid existing account, then create key record
    *       with that account as parent.
    *     If account_name is not set, then lookup account with key in the blockchain
    *       add contact account using data from blockchain and then set the private key
    */
   public_key_type  wallet::import_private_key( const private_key_type& key, 
                                                const string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto import_public_key = key.get_public_key();

      owallet_key_record current_key_record = my->_wallet_db.lookup_key( import_public_key );
      if( current_key_record.valid() )
      {
         current_key_record->encrypt_private_key( my->_wallet_password, key );
         my->_wallet_db.store_key( *current_key_record );
         return import_public_key;
      }
      else if( account_name == string() )
      {
         auto registered_account = my->_blockchain->get_account_record( import_public_key );
         FC_ASSERT( registered_account );
 
         add_contact_account( registered_account->name, import_public_key );
         return import_private_key( key, registered_account->name );
      }
      else
      {
         FC_ASSERT( is_valid_account_name( account_name ) );
         FC_ASSERT( is_valid_account( account_name ) );

         auto current_account = my->_wallet_db.lookup_account( account_name );

         FC_ASSERT( current_account.valid() );

         auto pub_key = key.get_public_key();
         address key_address(pub_key);
         auto current_key_record = my->_wallet_db.lookup_key( key_address );
         if( current_key_record.valid() )
         {
            FC_ASSERT( current_key_record->account_address == current_account->account_address );
            current_key_record->encrypt_private_key( my->_wallet_password, key );
            my->_wallet_db.store_key( *current_key_record );
            return current_key_record->public_key;
         }

         key_data new_key_data;
         if( current_account.valid() )
            new_key_data.account_address = current_account->account_address;
         new_key_data.encrypt_private_key( my->_wallet_password, key );

         my->_wallet_db.store_key( new_key_data );

         return pub_key;
      }

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   public_key_type wallet::import_wif_private_key( const string& wif_key, 
                                        const string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto wif_bytes = fc::from_base58(wif_key);
      auto key_bytes = vector<char>(wif_bytes.begin() + 1, wif_bytes.end() - 4);
      auto key = variant(key_bytes).as<private_key_type>();
      auto check = fc::sha256::hash( wif_bytes.data(), wif_bytes.size() - 4 );

      if( 0 == memcmp( (char*)&check, wif_bytes.data() + wif_bytes.size() - 4, 4 ) )
         return import_private_key( key, account_name );
      
      FC_ASSERT( false, "Error parsing WIF private key" );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }


   void  wallet::scan_chain( uint32_t start, uint32_t end, 
                             scan_progress_callback progress_callback )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      if( start == 0 )
      {
         scan_state();
         ++start;
      }

      auto min_end = std::min<size_t>( my->_blockchain->get_head_block_num(), end );

      auto account_priv_keys = my->_wallet_db.get_account_private_keys( my->_wallet_password );

      for( auto block_num = start; block_num <= min_end; ++block_num )
      {
         my->scan_block( block_num, account_priv_keys );
         if( progress_callback )
            progress_callback( block_num, min_end );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("start",start)("end",end) ) }


   void  wallet::sign_transaction( signed_transaction& trx, const std::unordered_set<address>& req_sigs )
   { try {
      for( auto addr : req_sigs )
      {
         trx.sign( get_private_key( addr ), my->_blockchain->chain_id()  );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("trx",trx)("req_sigs",req_sigs) ) }

   private_key_type wallet::get_private_key( const address& addr )const
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto key = my->_wallet_db.lookup_key( addr );
      FC_ASSERT( key.valid() );
      FC_ASSERT( key->has_private_key() );
      return key->decrypt_private_key( my->_wallet_password );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("addr",addr) ) }


   vector<pretty_transaction>         wallet::get_pretty_transaction_history( const string& account_name ) const
   {
       auto history = get_transaction_history( account_name );
       vector<pretty_transaction> pretties;
       pretties.reserve( history.size() );
       for (auto item : history)
           pretties.push_back( to_pretty_trx( item ) );
       return pretties;
   }

   /** 
    * @return the list of all transactions related to this wallet
    */
   vector<wallet_transaction_record>    wallet::get_transaction_history( const string& account_name )const
   { try {
      FC_ASSERT( is_open() );

      std::vector<wallet_transaction_record> recs;
      auto my_trxs = my->_wallet_db.transactions;
      recs.reserve( my_trxs.size() );

      public_key_type account_pub;
      if( account_name != string() )
         account_pub = get_account_public_key( account_name );

      for( auto iter : my_trxs)
      {
         if( account_name == string() || account_name == "*" ||
             (iter.second.to_account && *iter.second.to_account == account_pub) ||
             (iter.second.from_account && *iter.second.from_account == account_pub)  )
         {
            recs.push_back(iter.second);
         }
      }
    
      std::sort(recs.begin(), recs.end(), [](const wallet_transaction_record& a,
                                             const wallet_transaction_record& b)
                                           -> bool
               {
                   return a.received_time < b.received_time;
               });
      return recs;

   } FC_RETHROW_EXCEPTIONS( warn, "" ) }


   /**
    *  If this wallet has any delegate keys, this method will return the time
    *  at which this wallet may produce a block.
    */
   fc::time_point_sec wallet::next_block_production_time()const
   { try {
      auto now = fc::time_point(bts::blockchain::now());
      uint32_t interval_number = now.sec_since_epoch() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      uint32_t next_block_time = interval_number * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      if( next_block_time == my->_blockchain->now().sec_since_epoch() )
         next_block_time += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;

      uint32_t last_block_time = next_block_time + BTS_BLOCKCHAIN_NUM_DELEGATES * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;

      while( next_block_time < last_block_time )
      {
         auto id = my->_blockchain->get_signing_delegate_id( fc::time_point_sec( next_block_time ) );
         auto delegate_record = my->_blockchain->get_account_record( id );
         FC_ASSERT( delegate_record.valid(), "", ("delegate_id",id ) );
         auto key = my->_wallet_db.lookup_key( delegate_record->active_key() );
         if( key.valid() && key->has_private_key() )
         {
            return fc::time_point_sec( next_block_time ); 
         }
         next_block_time += BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      }
      return fc::time_point_sec();
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::sign_block( signed_block_header& header )const
   { try {
      FC_ASSERT( is_unlocked() );
      auto signing_delegate_id = my->_blockchain->get_signing_delegate_id( header.timestamp );
      auto delegate_rec = my->_blockchain->get_account_record( signing_delegate_id );

      auto delegate_pub_key = my->_blockchain->get_signing_delegate_key( header.timestamp );
      auto delegate_key = get_private_key( address(delegate_pub_key) );
      FC_ASSERT( delegate_pub_key == delegate_key.get_public_key() );

      header.previous_secret = my->get_secret( 
                                      delegate_rec->delegate_info->last_block_num_produced,
                                      delegate_key );

      auto next_secret = my->get_secret( my->_blockchain->get_head_block_num() + 1, delegate_key );
      header.next_secret_hash = fc::ripemd160::hash( next_secret );

      header.sign(delegate_key);
      FC_ASSERT( header.validate_signee( delegate_pub_key ) );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("header",header) ) }

   /**
    *  This method assumes that fees can be paid in the same asset type as the 
    *  asset being transferred so that the account can be kept private and 
    *  secure.
    *
    */
   vector<signed_transaction> wallet::multipart_transfer( share_type    amount_to_transfer,
                                                const string& amount_to_transfer_symbol,
                                                const string& from_account_name,
                                                const string& to_account_name,
                                                const string& memo_message,
                                                bool          sign )
   { try {
       FC_ASSERT( is_open() );
       FC_ASSERT( is_unlocked() );
       FC_ASSERT( my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) );
       FC_ASSERT( is_receive_account( from_account_name ) );
       FC_ASSERT( is_valid_account( to_account_name ) );
       FC_ASSERT( is_unique_account( to_account_name ), 
                  "Local account name conflicts with registered name, "
                  "please rename your local account before attempting a transfer",
                  ("to_account_name",to_account_name) );

       FC_ASSERT( memo_message.size() <= BTS_BLOCKCHAIN_MAX_MEMO_SIZE );
       FC_ASSERT( amount_to_transfer > get_priority_fee( amount_to_transfer_symbol ).amount );


       auto asset_id = my->_blockchain->get_asset_id( amount_to_transfer_symbol );
       asset asset_to_transfer( amount_to_transfer, asset_id );

       /**
        *  TODO: until we support paying fees in other assets, this will not function
        *  properly.
        */
       FC_ASSERT( asset_id == 0, "multipart transfers only support base shares",
                  ("asset_to_transfer",asset_to_transfer)("symbol",amount_to_transfer_symbol));

       vector<signed_transaction >       trxs;
       vector<share_type>                amount_sent;
       vector<wallet_balance_record>    balances_to_store; // records to cache if transfer succeeds

       public_key_type  receiver_public_key = get_account_public_key( to_account_name );
       private_key_type sender_private_key  = get_account_private_key( from_account_name );
       public_key_type  sender_public_key   = sender_private_key.get_public_key();
       address          sender_account_address( sender_private_key.get_public_key() );
       
       asset total_fee = get_priority_fee( amount_to_transfer_symbol );

       asset amount_collected( 0, asset_id );
       for( auto balance_item : my->_wallet_db.balances )
       {
          auto owner = balance_item.second.owner();
          if( balance_item.second.asset_id() == asset_id &&
              my->address_in_account( owner, sender_account_address ) )
          {
             signed_transaction trx;

             auto from_balance = balance_item.second.get_balance();

             if( from_balance.amount <= 0 ) 
                continue;

             trx.withdraw( balance_item.first,
                           from_balance.amount );

             from_balance -= total_fee;

             /** make sure there is at least something to withdraw at the other side */
             if( from_balance < total_fee )
                continue; // next balance_item

             asset amount_to_deposit( 0, asset_id );
             asset amount_of_change( 0, asset_id );

             if( (amount_collected + from_balance) > asset_to_transfer )
             {
                amount_to_deposit = asset_to_transfer - amount_collected;
                amount_of_change  = from_balance - amount_to_deposit;
                amount_collected += amount_to_deposit;
             }
             else
             {
                amount_to_deposit = from_balance;
             }

             if( amount_to_deposit.amount > 0 )
             {
                trx.deposit_to_account( receiver_public_key,
                                        amount_to_deposit,
                                        sender_private_key,
                                        memo_message,
                                        select_delegate_vote(),
                                        sender_private_key.get_public_key(),
                                        from_memo );
             }
             if( amount_of_change > total_fee )
             {
                trx.deposit_to_account( sender_public_key,
                                        amount_of_change,
                                        sender_private_key,
                                        memo_message,
                                        select_delegate_vote(),
                                        receiver_public_key,
                                        to_memo );

                /** randomly shuffle change to prevent analysis */
                if( rand() % 2 ) 
                {
                   FC_ASSERT( trx.operations.size() >= 3 )
                   std::swap( trx.operations[1], trx.operations[2] );
                }
             }

             // set the balance of this item to 0 so that we do not
             // attempt to spend it again.
             balance_item.second.balance = 0;
             balances_to_store.push_back( balance_item.second );


             if( sign )
             {
                auto owner_private_key = get_private_key( balance_item.second.owner() );
                trx.sign( owner_private_key, my->_blockchain->chain_id() );
             }

             trxs.emplace_back( trx );
             amount_sent.push_back( amount_to_deposit.amount );
             if( amount_collected >= asset( amount_to_transfer, asset_id ) )
                break;
          } // if asset id matches
       } // for each balance_item

       // If we went through all our balances and still don't have enough
       if (amount_collected < asset( amount_to_transfer, asset_id ))
       {
          FC_ASSERT( !"Insufficient funds.");
       }

       if( sign ) // don't store invalid trxs..
       {
          for( auto rec : balances_to_store )
          {
              my->_wallet_db.store_balance( rec );
          }
          for( uint32_t i =0 ; i < trxs.size(); ++i )
             my->_wallet_db.cache_transaction( trxs[i], asset( -amount_sent[i], asset_id), 
                                               total_fee.amount, 
                                               memo_message, 
                                               receiver_public_key,
                                               bts::blockchain::now(),
                                               bts::blockchain::now(),
                                               sender_public_key
                                             );
       }

       return trxs;
      
   } FC_RETHROW_EXCEPTIONS( warn, "", 
         ("amount_to_transfer",amount_to_transfer)
         ("amount_to_transfer_symbol",amount_to_transfer_symbol)
         ("from_account_name",from_account_name)
         ("to_account_name",to_account_name)
         ("memo_message",memo_message) ) }


   signed_transaction   wallet::withdraw_delegate_pay( const string& delegate_name,
                                                       share_type amount_to_withdraw,
                                                       const string& withdraw_to_account_name,
                                                       const string& memo_message,
                                                       bool sign )
   { try {
       FC_ASSERT( is_open() );
       FC_ASSERT( is_unlocked() );
       FC_ASSERT( is_receive_account( delegate_name ) );
       FC_ASSERT( is_valid_account( withdraw_to_account_name ) );

       auto delegate_account_record = my->_blockchain->get_account_record( delegate_name ); //_wallet_db.lookup_account( delegate_name );
       FC_ASSERT( delegate_account_record.valid() );
       FC_ASSERT( delegate_account_record->is_delegate() );

       auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );
       FC_ASSERT( delegate_account_record->delegate_info->pay_balance >= (amount_to_withdraw + required_fees.amount), "",
                  ("delegate_account_record",delegate_account_record));

       signed_transaction trx;
       unordered_set<address> required_signatures;

       owallet_key_record  delegate_key = my->_wallet_db.lookup_key( delegate_account_record->active_key() );
       FC_ASSERT( delegate_key && delegate_key->has_private_key() );
       auto delegate_private_key = delegate_key->decrypt_private_key( my->_wallet_password );
       required_signatures.insert( delegate_private_key.get_public_key() );

       public_key_type  receiver_public_key = get_account_public_key( withdraw_to_account_name );
       
       trx.withdraw_pay( delegate_account_record->id, amount_to_withdraw + required_fees.amount );
       trx.deposit_to_account( receiver_public_key,
                               asset(amount_to_withdraw,0),
                               delegate_private_key,
                               memo_message,
                               delegate_account_record->id, // vote for yourself
                               delegate_private_key.get_public_key(),
                               from_memo );

       if( sign )
       {
          delegate_account_record->delegate_info->pay_balance -= amount_to_withdraw;
          // my->_wallet_db.cache_account( *delegate_account_record );
          sign_transaction( trx, required_signatures );
       }
       return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name)
                                      ("amount_to_withdraw",amount_to_withdraw ) ) }

   signed_transaction   wallet::transfer_asset( share_type amount_to_transfer,
                                        const string& amount_to_transfer_symbol,
                                        const string& from_account_name,
                                        const string& to_account_name,
                                        const string& memo_message,
                                        bool sign )
   { try {
       FC_ASSERT( is_open() );
       FC_ASSERT( is_unlocked() );
       FC_ASSERT( my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) );
       FC_ASSERT( is_receive_account( from_account_name ) );
       FC_ASSERT( is_valid_account( to_account_name ) );
       FC_ASSERT( is_unique_account( to_account_name ), 
                  "Local account name conflicts with registered name, "
                  "please rename your local account before attempting a transfer",
                  ("to_account_name",to_account_name) );

      auto asset_id = my->_blockchain->get_asset_id( amount_to_transfer_symbol );
      asset asset_to_transfer( amount_to_transfer, asset_id );


      public_key_type  receiver_public_key = get_account_public_key( to_account_name );
      private_key_type sender_private_key  = get_account_private_key( from_account_name );
      public_key_type  sender_public_key   = sender_private_key.get_public_key();
      address          sender_account_address( sender_private_key.get_public_key() );

      signed_transaction     trx;
      unordered_set<address> required_signatures;


      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );
      if( required_fees.asset_id == asset_to_transfer.asset_id )
      {
         my->withdraw_to_transaction( required_fees.amount + amount_to_transfer,
                                      required_fees.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );
      }
      else
      {
         my->withdraw_to_transaction( amount_to_transfer,
                                      asset_to_transfer.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );

         my->withdraw_to_transaction( required_fees.amount,
                                      required_fees.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );
      }

      trx.deposit_to_account( receiver_public_key,
                              asset_to_transfer,
                              sender_private_key,
                              memo_message,
                              select_delegate_vote(),
                              sender_private_key.get_public_key(),
                              from_memo );

      if( sign )
      {
          sign_transaction( trx, required_signatures );
          my->_wallet_db.cache_transaction( trx, asset_to_transfer,
                                            required_fees.amount, 
                                            memo_message, 
                                            receiver_public_key,
                                            bts::blockchain::now(),
                                            bts::blockchain::now(),
                                            sender_public_key
                                          );
      }
      return trx; 

   } FC_RETHROW_EXCEPTIONS( warn, "", 
         ("amount_to_transfer",amount_to_transfer)
         ("amount_to_transfer_symbol",amount_to_transfer_symbol)
         ("from_account_name",from_account_name)
         ("to_account_name",to_account_name)
         ("memo_message",memo_message) ) }


   wallet_transaction_record wallet::register_account( const string& account_to_register,
                                                const variant& public_data,
                                                bool as_delegate,
                                                const string& pay_with_account_name,
                                                const bool sign )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_to_register ) );

      auto registered_account = my->_blockchain->get_account_record( account_to_register );
      FC_ASSERT( !registered_account.valid(), "the account has already been registered",
                 ("account_to_register",account_to_register) );

      auto payer_public_key = get_account_public_key( pay_with_account_name );
      address from_account_address( payer_public_key );

      auto opt_account = get_account( account_to_register );
      FC_ASSERT( opt_account.valid() );
      auto account_public_key = get_account_public_key( account_to_register );

      signed_transaction     trx;
      unordered_set<address> required_signatures;

      trx.register_account( account_to_register, public_data,
                            account_public_key, // master
                            account_public_key, // active
                            as_delegate );

      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );

      if( as_delegate )
      {
        required_fees += asset(my->_blockchain->get_delegate_registration_fee(),0);
      }

      auto size_fee = fc::raw::pack_size( public_data );
      required_fees += asset((my->_blockchain->get_fee_rate() * size_fee)/1000 );

      // TODO: adjust fee based upon blockchain price per byte and
      // the size of trx... 'recursivey'

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   from_account_address,
                                   trx, required_signatures );
      if( sign )
      {
         sign_transaction( trx, required_signatures );
         return my->_wallet_db.cache_transaction( trx, 
                                                  asset(), 
                                                  required_fees.amount, 
                                                  "register " + account_to_register + (as_delegate? " as a delegate" : ""), 
                                                  payer_public_key, 
                                                  bts::blockchain::now(),
                                                  bts::blockchain::now(),
                                                  payer_public_key
                                                );
      }
      return wallet_transaction_record(transaction_data(trx));

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_to_register",account_to_register)
                                      ("public_data", public_data)
                                      ("pay_with_account_name", pay_with_account_name) 
                                      ("as_delegate",as_delegate) ) }




   signed_transaction  wallet::create_asset( const string& symbol,
                                             const string& asset_name,
                                             const string& description,
                                             const variant& data,
                                             const string& issuer_account_name,
                                             share_type max_share_supply, 
                                             const bool sign  )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( issuer_account_name ) );
      FC_ASSERT( ! my->_blockchain->is_valid_symbol( symbol ) ); // not yet registered
      //TODO rename "is_valid_symbol" to "is_registered_symbol"
      //TODO "is_valid_symbol" will actually check if the string is valid for symbol name

      signed_transaction     trx;
      unordered_set<address> required_signatures;


      // TODO: adjust fee based upon blockchain price per byte and
      // the size of trx... 'recursivey'
      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );

      auto size_fee = fc::raw::pack_size( data );
      required_fees += asset((my->_blockchain->get_fee_rate() * size_fee)/1000 );
      required_fees += asset(my->_blockchain->get_asset_registration_fee(),0);

      auto from_account_address = get_account_public_key( issuer_account_name );
      auto oname_rec = my->_blockchain->get_account_record( issuer_account_name );
      FC_ASSERT( oname_rec.valid() );

      required_signatures.insert( address( from_account_address ) );

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   from_account_address,
                                   trx, required_signatures );
    
      trx.create_asset( symbol, asset_name,
                        description, data,
                        oname_rec->id, max_share_supply );

      if( sign )
         sign_transaction( trx, required_signatures );

      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol)
                                      ("name", asset_name )
                                      ("description", description)
                                      ( "issuer_account", issuer_account_name) ) }

   signed_transaction  wallet::issue_asset( share_type amount_to_issue, 
                                            const string& symbol,                                               
                                            const string& to_account_name,
                                            const string& memo_message,
                                            const bool sign )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( to_account_name ) );
      FC_ASSERT( my->_blockchain->is_valid_symbol( symbol ) );

      signed_transaction         trx;
      unordered_set<address>     required_signatures;
      
      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );

      auto asset_record = my->_blockchain->get_asset_record( symbol );
      FC_ASSERT(asset_record.valid(), "no such asset record");
      auto issuer_account = my->_blockchain->get_account_record( asset_record->issuer_account_id );
      FC_ASSERT(issuer_account, "uh oh! no account for valid asset");

      asset shares_to_issue( amount_to_issue, asset_record->id );
      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( issuer_account->name ),
                                   trx, required_signatures );
     
      trx.issue( shares_to_issue );
      required_signatures.insert( issuer_account->active_key() );

      public_key_type  receiver_public_key = get_account_public_key( to_account_name );
      owallet_account_record issuer = my->_wallet_db.lookup_account( asset_record->issuer_account_id );
      FC_ASSERT( issuer.valid() );
      owallet_key_record  issuer_key = my->_wallet_db.lookup_key( issuer->account_address );
      FC_ASSERT( issuer_key && issuer_key->has_private_key() );
      auto sender_private_key = issuer_key->decrypt_private_key( my->_wallet_password );

      trx.deposit_to_account( receiver_public_key,
                              shares_to_issue,
                              sender_private_key,
                              memo_message,
                              0,
                              sender_private_key.get_public_key(),
                              from_memo );

      if( sign )
          sign_transaction( trx, required_signatures );

      return trx;
   }


   wallet_transaction_record wallet::update_registered_account( const string& account_to_update,
                                                                const string& pay_from_account,
                                                                optional<variant> public_data,
                                                                optional<public_key_type> new_active_key,
                                                                bool as_delegate,
                                                                const bool sign )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_to_update ) );
      FC_ASSERT( is_valid_account_name( pay_from_account ) );

      signed_transaction trx;
      unordered_set<address>     required_signatures;
      auto payer_public_key = get_account_public_key( pay_from_account );

      auto account = my->_blockchain->get_account_record( account_to_update );
      FC_ASSERT(account.valid(), "No such account: ${acct}", ("acct", account_to_update));
      
      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );

      if( as_delegate && !account->is_delegate() )
      {
        required_fees += asset(my->_blockchain->get_delegate_registration_fee(),0);
      }

      auto size_fee = fc::raw::pack_size( public_data );
      required_fees += asset((my->_blockchain->get_fee_rate() * size_fee)/1000 );

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   payer_public_key,
                                   trx, required_signatures );
     
      required_signatures.insert( account->active_key() ); 
    
      trx.update_account( account->id, public_data, new_active_key, as_delegate );
       
      if (sign)
      {
          sign_transaction( trx, required_signatures );
          return my->_wallet_db.cache_transaction( trx, 
                                                  asset(), 
                                                  required_fees.amount, 
                                                  "update " + account_to_update + (as_delegate? " as a delegate" : ""), 
                                                  payer_public_key, 
                                                  bts::blockchain::now(),
                                                  bts::blockchain::now(),
                                                  payer_public_key
                                                );
      }
      return wallet_transaction_record(transaction_data(trx));

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_to_update",account_to_update)
                                      ("pay_from_account",pay_from_account)
                                      ("public_data",public_data)
                                      ("new_active_key",new_active_key)
                                      ("as_delegate",as_delegate)
                                      ("sign",sign) ) }


   signed_transaction wallet::create_proposal( const string& delegate_account_name,
                                       const string& subject,
                                       const string& body,
                                       const string& proposal_type,
                                       const variant& data,
                                       const bool sign  )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( delegate_account_name ) );
      // TODO validate subject, body, and data

      signed_transaction trx;
      unordered_set<address>     required_signatures;

      auto delegate_account = my->_blockchain->get_account_record( delegate_account_name );
      FC_ASSERT(delegate_account.valid(), "No such account: ${acct}", ("acct", delegate_account_name));
      
      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );

      trx.submit_proposal( delegate_account->id, subject, body, proposal_type, data );
      required_fees += asset( (fc::raw::pack_size(trx) * my->_blockchain->get_fee_rate())/1000, 0 );

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( delegate_account->name ),
                                   trx, required_signatures );
     
      required_signatures.insert( delegate_account->active_key() ); 
    
       
      if (sign)
          sign_transaction( trx, required_signatures );

      return trx;
   }

   signed_transaction wallet::vote_proposal( const string& delegate_name, 
                                             proposal_id_type proposal_id, 
                                             proposal_vote::vote_type vote,
                                             const string& message,
                                             const bool sign )
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( delegate_name ) );
      // TODO validate subject, body, and data

      signed_transaction trx;
      unordered_set<address>     required_signatures;

      auto delegate_account = my->_blockchain->get_account_record( delegate_name );
      FC_ASSERT(delegate_account.valid(), "No such account: ${acct}", ("acct", delegate_account));
      FC_ASSERT(delegate_account->is_delegate());

      bool found_active_delegate = false;
      auto next_active = my->_blockchain->next_round_active_delegates();
      for( auto delegate_id : next_active )
      {
         if( delegate_id == delegate_account->id )
         {
            found_active_delegate = true;
            break;
         }
      }
      FC_ASSERT( found_active_delegate, "Delegate ${name} is not currently active",
                 ("name",delegate_name) );
      

      FC_ASSERT(message.size() < BTS_BLOCKCHAIN_PROPOSAL_VOTE_MESSAGE_MAX_SIZE );
      trx.vote_proposal( proposal_id, delegate_account->id, vote, message );

      auto required_fees = get_priority_fee( BTS_ADDRESS_PREFIX );
      required_fees += asset( (fc::raw::pack_size(trx) * my->_blockchain->get_fee_rate())/1000, 0 );
      
      /*
      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( account->name ),
                                   trx, required_signatures );
      */

      trx.withdraw_pay( delegate_account->id, required_fees.amount );
      required_signatures.insert( delegate_account->active_key() ); 
       
      if( sign )
      {
          sign_transaction( trx, required_signatures );
      }

      return trx;
   }

   asset wallet::get_priority_fee( const string& symbol )const
   {
      // TODO: support price conversion using price from blockchain
      return asset( 10000, 0 ); // TODO: actually read the value set
   }

   
   pretty_transaction wallet::to_pretty_trx( const wallet_transaction_record& trx_rec ) const
   {
      auto pretty_trx = pretty_transaction();
     
      auto trx = trx_rec.trx;
      auto loc = my->_blockchain->get_transaction_location( trx.id() );
   
      if (loc)
      {
          pretty_trx.block_num = loc->block_num;
          pretty_trx.trx_num = loc->trx_num;
      } 
      else /* to_pretty_trx will often be called for transactions that are not in the chain yet */ 
      { 
          pretty_trx.block_num = -1;
          pretty_trx.trx_num = -1;
      }

      pretty_trx.trx_id = trx.id();
      pretty_trx.received_time = trx_rec.received_time.sec_since_epoch();
      pretty_trx.created_time = trx_rec.created_time.sec_since_epoch();
      pretty_trx.amount = trx_rec.amount;
      pretty_trx.fees = trx_rec.fees;
      pretty_trx.memo_message = trx_rec.memo_message;

      pretty_trx.to_me = false;
      pretty_trx.from_me = false;


      pretty_trx.from_account = "";
      if( trx_rec.from_account )
      {
          auto acct_record = my->_wallet_db.lookup_account( *trx_rec.from_account );
          if (acct_record)
          {
              pretty_trx.from_me = my->_wallet_db.has_private_key( address( *trx_rec.from_account ) );
              pretty_trx.from_account = acct_record->name;
          }
          else
          {
              auto registered_account = my->_blockchain->get_account_record( *trx_rec.from_account );
              if( registered_account.valid() )
                 pretty_trx.from_account = registered_account->name;
              else
                 pretty_trx.from_account = string( *trx_rec.from_account );
          }
      }
      else 
      {
         pretty_trx.from_account = "unknown";
      }

      pretty_trx.to_account = "";
      if( trx_rec.to_account )
      {

          auto acct_record = my->_wallet_db.lookup_account( *trx_rec.to_account );
          if (acct_record)
          {
              pretty_trx.to_me = my->_wallet_db.has_private_key( address( *trx_rec.to_account ) );
              pretty_trx.to_account = acct_record->name;
          }
          else
          {
              auto registered_account = my->_blockchain->get_account_record( *trx_rec.to_account );
              if( registered_account.valid() )
                 pretty_trx.to_account = registered_account->name;
              else
                 pretty_trx.to_account = string( *trx_rec.to_account );
          }
      }
      else
      {
         pretty_trx.to_account = "unknown";
      }

      for( auto op : trx.operations )
      {
          switch( operation_type_enum( op.type ) )
          {
              case (withdraw_op_type):
              {
                  auto pretty_op = pretty_withdraw_op();
                  auto withdraw_op = op.as<withdraw_operation>();
                  /*
                  auto name = std::string("");
                  address owner;
                  auto balance_rec = my->_blockchain->get_balance_record( withdraw_op.balance_id );
                  if (balance_rec)
                  {
                      owner = balance_rec->owner();
                      if (owner) 
                      {
                          owallet_account_record acc_rec = my->_wallet_db.lookup_account( owner );
                          if ( acc_rec )
                              name = acc_rec->name;
                      }
                  }
                  
                  pretty_op.owner = std::make_pair(owner, name);
                  pretty_op.amount = withdraw_op.amount;
                  */
                  pretty_trx.add_operation(pretty_op);
                  break;
              }
              case (deposit_op_type):
              {
                  auto pretty_op = pretty_deposit_op();
                  auto deposit_op = op.as<deposit_operation>();
/*
                  // TODO
                  name_id_type vote = deposit_op.condition.delegate_id;
                  name_id_type pos_delegate_id = (vote > 0) ? vote : name_id_type(-vote);
                  int32_t delegate_account_num = my->_wallet_db.account_id_to_account[pos_delegate_id];
                  blockchain::oaccount_record delegate_acct_rec = my->_blockchain->get_account_record( delegate_account_num );
                  string delegate_name = delegate_acct_rec ? delegate_acct_rec->name : "";
                  pretty_op.vote = std::make_pair(vote, delegate_name);

                  if( withdraw_condition_types( deposit_op.condition.type ) == withdraw_signature_type )
                  {
                      auto name = std::string("");
                      auto condition = deposit_op.condition.as<withdraw_with_signature>();
                      auto acc_rec = my->_wallet_db.lookup_account( condition.owner );
                      if ( acc_rec )
                          name = acc_rec->name;
                      pretty_op.owner = std::make_pair( condition.owner, name );
                  }
                  else
                  {
                      FC_ASSERT(false, "Unimplemented withdraw condition: ${c}",
                                      ("c", deposit_op.condition.type));
                  }

                  pretty_op.amount = deposit_op.amount;
*/
                  pretty_trx.add_operation(pretty_op);
                  break;
              }
              case( register_account_op_type ):
              {
                  auto reserve_name_op = op.as<register_account_operation>();
                  auto pretty_op = pretty_reserve_name_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( update_account_op_type ):
              {
                  auto update_name_op = op.as<update_account_operation>();
                  auto pretty_op = pretty_update_name_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( create_asset_op_type ):
              {
                  //auto create_asset_op = op.as<create_asset_operation>();
                  auto pretty_op = pretty_create_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( update_asset_op_type ):
              {
                  //auto update_asset_op = op.as<update_asset_operation>();
                  auto pretty_op = pretty_update_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( issue_asset_op_type ):
              {
                  //auto issue_asset_op = op.as<issue_asset_operation>();
                  auto pretty_op = pretty_issue_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( submit_proposal_op_type ):
              {
                  //auto submit_proposal_op = op.as<submit_proposal_operation>();
                  auto pretty_op = pretty_submit_proposal_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( vote_proposal_op_type ):
              {
                  //auto vote_proposal_op = op.as<vote_proposal_operation>();
                  auto pretty_op = pretty_vote_proposal_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case withdraw_pay_op_type:
              {
                 pretty_trx.add_operation( op );
                 break;
              }
              default:
              {
                  FC_ASSERT(false, "Unimplemented display op type: ${type}", ("type", op.type));
                  break;
              }
          } // switch op_type
      } // for op in trx
     
      return pretty_trx;
   }

   void wallet::import_bitcoin_wallet( const path& wallet_dat,
                                     const string& wallet_dat_passphrase,
                                     const string& account_name )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_name ) );
      FC_ASSERT( false, "Not Implemented" );
   } FC_RETHROW_EXCEPTIONS( warn, "error importing bitcoin wallet ${wallet_dat}", 
                            ("wallet_dat",wallet_dat)("account_name",account_name) ) }

   vector<asset> wallet::get_balance( const string& symbol, 
                              const string& account_name )const
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( account_name == "*" || is_valid_account_name( account_name ) );

      asset_id_type filter_asset_id;
      if( symbol != "*" )
      {
         auto opt_asset_record = my->_blockchain->get_asset_record( symbol );
         FC_ASSERT( opt_asset_record.valid(), "Unable to find asset for symbol '${s}'", ("s",symbol) );
         filter_asset_id = opt_asset_record->id;
      }


      address filter_address;

      if( account_name != "*" )
      {
         auto war = my->_wallet_db.lookup_account( account_name );
         FC_ASSERT( war.valid(), "Unable to find Wallet Account '${account_name}'", ("account_name",account_name) );

         auto account_key = my->_wallet_db.lookup_key( war->account_address );
         FC_ASSERT( account_key.valid() && account_key->has_private_key(), "Not your account" );
         //ilog( "account: ${war}", ("war",war) );
        // ilog( "account key: ${key}", ("key",account_key) );
         filter_address = war->account_address;
      }

      map<asset_id_type,share_type> balances;
      for( auto b : my->_wallet_db.balances )
      {
            auto key_rec = my->_wallet_db.lookup_key( b.second.owner() );
            if( key_rec.valid() && (account_name == std::string("*") || 
                                    key_rec->account_address == filter_address) )
            {
               auto bal = b.second.get_balance();
               if( symbol == "*" ||  bal.asset_id == filter_asset_id )
                  balances[bal.asset_id] += bal.amount;
            }
      };
      vector<asset> result;
      result.reserve( balances.size() );
      for( auto item : balances )
         result.push_back( asset( item.second, item.first ) );
      if( result.size() == 0 )
         result.push_back( asset() );
      return result;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol)("account_name",account_name) ) }

   vector<string> wallet::list() const
   {
       vector<string> wallets;
       auto path = get_data_directory();
       fc::directory_iterator end_itr; // constructs terminator
       for( fc::directory_iterator itr( path ); itr != end_itr; ++itr)
       {
          if (fc::is_directory( *itr ))
          {
              wallets.push_back( (*itr).stem().string() );
          }
       }
       return wallets;
   }

   bool  wallet::is_sending_address( const address& addr )const
   { try {
      return !is_receive_address( addr );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }


   bool  wallet::is_receive_address( const address& addr )const
   {  try {
      auto key_rec = my->_wallet_db.lookup_key( addr );
      if( key_rec.valid() )
         return key_rec->has_private_key();
      return false;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   vector<wallet_account_record> wallet::list_contact_accounts() const
   { try {
      vector<wallet_account_record> contact_accounts;
      unordered_map<int32_t, wallet_account_record> accs = my->_wallet_db.accounts;
      contact_accounts.reserve( my->_wallet_db.accounts.size() );
      for( auto item : my->_wallet_db.accounts )
      {
         if ( ! my->_wallet_db.has_private_key( item.second.account_address ) )
         {
            contact_accounts.push_back( item.second );
         }
      }
      return contact_accounts;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   vector<wallet_account_record> wallet::list_receive_accounts() const
   { try {

      vector<wallet_account_record> receive_accounts;
      unordered_map<int32_t, wallet_account_record> accs = my->_wallet_db.accounts;
      receive_accounts.reserve( my->_wallet_db.accounts.size() );
      for( auto item : my->_wallet_db.accounts )
      {
         if ( my->_wallet_db.has_private_key( item.second.account_address ) )
         {
            receive_accounts.push_back( item.second );
         }
      }
      return receive_accounts;

   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::clear_pending_transactions()
   {
      my->clear_pending_transactions();
   }

   void  wallet::scan_state()
   { try {
      ilog( "WALLET: scaning blockchain state" );
      my->scan_balances();
      my->scan_registered_accounts();
   } FC_RETHROW_EXCEPTIONS( warn, "" )  }

   /**
    *  A valid account is any named account registered in the blockchain or
    *  any local named account.
    */
   bool wallet::is_valid_account( const string& account_name )const
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( account_name ) );
      if( my->_wallet_db.lookup_account( account_name ).valid() )
          return true;
      return my->_blockchain->get_account_record( account_name ).valid();
   }

   /**
    *  Any account for which this wallet owns the private key.
    */
   bool wallet::is_receive_account( const string& account_name )const
   {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( account_name ) );
      auto opt_account = my->_wallet_db.lookup_account( account_name );
      if( !opt_account.valid() ) return false;
      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      if( !opt_key.valid()  ) return false;
      return opt_key->has_private_key();
   }

   /**
    * Account names are limited the same way as domain names.
    */
   bool wallet::is_valid_account_name( const string& account_name )const
   {
      return blockchain::account_record::is_valid_name( account_name );
   }


   private_key_type wallet::get_account_private_key( const string& account_name )const
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( is_valid_account_name( account_name ) );
      auto opt_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( opt_account.valid(), "Unable to find account '${name}'", 
                ("name",account_name) );

      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      FC_ASSERT( opt_key.valid(), "Unable to find key for account '${name}", 
                ("name",account_name) );

      FC_ASSERT( opt_key->has_private_key() );
      return opt_key->decrypt_private_key( my->_wallet_password );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   bool wallet::is_unique_account( const string& account_name )const
   {
      auto local_account      = my->_wallet_db.lookup_account( account_name );
      auto registered_account = my->_blockchain->get_account_record( account_name );
      if( local_account && registered_account )
         return local_account->account_address == address( registered_account->active_key() );
      return local_account || registered_account;
   }

   /**
    *  Looks up the public key for an account whether local or in the blockchain, with
    *  the local name taking priority.
    */
   public_key_type  wallet::get_account_public_key( const string& account_name )const
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_valid_account_name( account_name ) );

      auto opt_account = my->_wallet_db.lookup_account( account_name );
      if( !opt_account.valid() )
      {
         auto registered_account = my->_blockchain->get_account_record( account_name );
         if( registered_account.valid() )
            return registered_account->active_key();
      }
      FC_ASSERT( opt_account.valid(), "Unable to find account '${name}'", 
                ("name",account_name) );

      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      FC_ASSERT( opt_key.valid(), "Unable to find key for account '${name}", 
                ("name",account_name) );

      return opt_key->public_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   account_id_type wallet::select_delegate_vote()const
   {
      vector<account> for_candidates;
      vector<account> against_candidates;
      vector<account_id_type> active_delegates =
          my->_blockchain->get_delegates_by_vote(0, BTS_BLOCKCHAIN_NUM_DELEGATES);

      for (auto acct_rec : my->_wallet_db.accounts)
      {
         if (acct_rec.second.trust_level > 0)
             for_candidates.push_back(acct_rec.second);
         if (acct_rec.second.trust_level < 0)
             against_candidates.push_back(acct_rec.second);
      }
      if ( against_candidates.size() > 0 )
      {
         for (auto delegate_id : active_delegates)
            for (auto against_acct : against_candidates)
                if( against_acct.id== delegate_id )
                {
                    if (delegate_id == 0)
                    {
                        FC_ASSERT(!"WARNING - delegate id 0 bug @ 1");
                        return (rand() % BTS_BLOCKCHAIN_NUM_DELEGATES) + 1;
                    }
                    return -delegate_id;
                }
      }
      if( for_candidates.size() > 0 )
      {
         // find first delegate who is not active
         bool active = false;
         for (auto for_acct : for_candidates)
         {
            for (auto delegate_id : active_delegates)
            {
                if (for_acct.id== delegate_id)
                {
                    active = true;
                    break;
                }
            }
            if (active)
            {
                active = false;
                continue;
            }
            else
            {
                if (for_acct.id == 0)
                {
                    FC_ASSERT(!"WARNING - delegate id 0 bug @ 2");
                    return (rand() % BTS_BLOCKCHAIN_NUM_DELEGATES) + 1;
                }
                else
                    return for_acct.id;
            }
         }
         // all of our delegates are active - pick the one with the lowest vote
         int64_t min = std::numeric_limits<int64_t>::max();
         account_id_type winner;
         for( auto candidate : for_candidates )
         {
            auto acct_rec = my->_blockchain->get_account_record( candidate.id);
            FC_ASSERT(acct_rec);
            if (acct_rec->net_votes() < min)
            {
                min = acct_rec->net_votes();
                winner = acct_rec->id;
            }
         }
         if (winner == 0)
         {
            FC_ASSERT(!"WARNING - delegate id 0 bug @ 3");
            return (rand() % BTS_BLOCKCHAIN_NUM_DELEGATES) + 1;
         }
         else
            return winner;
      }
      else
      {
            auto num = rand();
            if (active_delegates[(num % BTS_BLOCKCHAIN_NUM_DELEGATES)] == 0)
            {
                FC_ASSERT(!"WARNING - delegate id 0 bug @ 4");
                return (rand() % BTS_BLOCKCHAIN_NUM_DELEGATES) + 1;
            }
            else
            {
                return active_delegates[(num % BTS_BLOCKCHAIN_NUM_DELEGATES)];
            }
      }
   }

   void      wallet::set_delegate_trust_level( const string& delegate_name, 
                                               int32_t trust_level)
   { try {
      FC_ASSERT( is_open() );
      auto war = my->_wallet_db.lookup_account( delegate_name );
      if( war.valid() )
      {
         war->trust_level = trust_level;
         my->_wallet_db.cache_account( *war );
      }
      else
      {
         auto reg_account = my->_blockchain->get_account_record( delegate_name );
         if( !reg_account.valid() )
         {
            FC_ASSERT( !"Not a Registered Account" );
         }
         if( !reg_account->is_delegate() )
         {
            FC_ASSERT( !"Account not registered as a delegate" );
         }
         add_contact_account( delegate_name, reg_account->active_key() );
         set_delegate_trust_level( delegate_name, trust_level );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name)
                                      ("trust_level",  trust_level) ) }

   int32_t   wallet::get_delegate_trust_level( const string& delegate_name) const
   { try {
      FC_ASSERT( is_open() );
      auto war = my->_wallet_db.lookup_account( delegate_name );
      if( war.valid() )
         return war->trust_level;
      return 0;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name) ) }

   vector<wallet_balance_record>  wallet::get_unspent_balances( const string& account_name,
                                                               const string& symbol ) const
   { try {
      vector<wallet_balance_record> balances;
      for( auto& balance_item : my->_wallet_db.balances )
      {
    /*     auto owner = balance_item.second.owner();
         if( balance_item.second.asset_id() == asset_id && 
             address_in_account( owner, from_account_address ) )
         {
    */
            if( balance_item.second.balance > 0 )
            {
               balances.push_back( balance_item.second );
            }
    //    }
      }
      return balances;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name)("symbol",symbol) ) }


   optional<wallet_account_record>         wallet::get_account_record( const address& addr)const
   {
      return my->_wallet_db.lookup_account( addr );
   }

   wallet::account_balance_summary_type    wallet::get_account_balances()const
   { try {
      account_balance_summary_type result;
      unordered_map< address, unordered_map< asset_id_type, share_type> > raw_results;
      for( auto b : my->_wallet_db.balances )
      {
          auto okey_rec = my->_wallet_db.lookup_key( b.second.owner() );
          if( okey_rec && okey_rec->has_private_key() )
          {
             asset bal = b.second.get_balance();
             raw_results[ okey_rec->account_address ][ bal.asset_id ] += bal.amount;
          }
      }
      for( auto account : raw_results )
      {
         auto oaccount = my->_wallet_db.lookup_account( account.first );
         string name = oaccount ? oaccount->name : string(account.first);
         for( auto item : account.second )
         {
            string symbol = my->_blockchain->get_asset_symbol( item.first );
            result[name][symbol] = item.second;
         }
      }
      ilog( "${r}", ("r",result) );
      return result;
   } FC_RETHROW_EXCEPTIONS(warn,"") }
} } // bts::wallet

