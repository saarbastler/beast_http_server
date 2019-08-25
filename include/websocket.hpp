#ifndef _INCLUDE_WEBSOCKET_SESSION_H_
#define _INCLUDE_WEBSOCKET_SESSION_H_

#include <boost/beast/websocket.hpp>
#include <mutex>
#include <atomic>
#include <queue>
#include <vector>

#include <base.hpp>


namespace http {

  template<class T>
  class websocket_list : public std::enable_shared_from_this<websocket_list<T>>
  {
  public:
    using Callback = std::function<void(T&)>;
    using DataReceived = std::function<void(T&, const std::string&)>;

    void registerWebsocket(const std::shared_ptr<T>& websocket)
    {
      checkList();

      websockets.emplace_back(websocket);

      std::cout << "Websocket registered: " << websockets.back().lock()->remote() << std::endl;

      for (auto it : callbacks)
        it(*websocket);
    }

    void checkList()
    {
      auto before = websockets.size();

      websockets.erase( std::remove_if(websockets.begin(), websockets.end(), [](std::weak_ptr<T>& elem)
      {
        return elem.expired();
      }), websockets.end());

      if( before != websockets.size())
        std::cout << "removed " << (before - websockets.size()) << " Websockets, new size: " << websockets.size() << std::endl;
    }

    void dataReceived(std::shared_ptr<T>&& websocket)
    {
      const std::string& data{ boost::beast::buffers_to_string(websocket->buffer().data()) };

      for (auto it : dataReceivedList)
        it(*websocket, data);

      //std::cout << "WS received from " << websocket->remote() << ": " << data /*boost::beast::buffers(websocket->buffer().data())*/ << std::endl;
    }

    void sendToAll(const std::string& txt) 
    {
      wstext= txt;
      for (auto it = websockets.begin(); it != websockets.end(); it++)
      {
        if (!it->expired())
          it->lock()->send(wstext);
      }
    }

    void registerCallback(Callback&& callback)
    {
      callbacks.emplace_back(std::move(callback));
    }

    void registerDataReceived(DataReceived &&dr)
    {
      dataReceivedList.emplace_back(std::move(dr));
    }

  private:

    std::vector<Callback> callbacks;
    std::vector<DataReceived> dataReceivedList;
    std::vector<std::weak_ptr<T>> websockets;
    std::string wstext;
  };

  class websocket_session;
  using ws_list = websocket_list<websocket_session>;
  using ws_list_ptr = std::shared_ptr<ws_list>;

  // Echoes back all received WebSocket messages
  class websocket_session : public std::enable_shared_from_this<websocket_session>
  {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::steady_timer timer_;
    boost::beast::multi_buffer buffer_;
    char ping_state_ = 0;

    ws_list_ptr websocketList_;
    std::string uri_;
    std::string remote_;

    std::queue<std::string> sendFifo;
    std::mutex fifoMutex;
    std::atomic<bool> writeInProgress;

  public:
    // Take ownership of the socket
    explicit
      websocket_session(boost::asio::ip::tcp::socket socket, const ws_list_ptr& websocketList)
      : ws_(std::move(socket))
      , websocketList_ {websocketList}
      , strand_(ws_.get_executor())
      , timer_(ws_.get_executor().context(),
      (std::chrono::steady_clock::time_point::max)())
    {
      writeInProgress = false;
    }

    ~websocket_session()
    {
      std::cout << "Websocket destroyed: " << remote_ << std::endl;

      websocketList_->checkList();
    }

    boost::string_view uri()
    {
      return boost::string_view(uri_);
    }

    boost::string_view remote()
    {
      return boost::string_view(remote_);
    }

    const boost::beast::multi_buffer& buffer()
    {
      return buffer_;
    }

    // Start the asynchronous operation
    template<class Body, class Allocator>
    void
      do_accept(boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> req)
    {
      // Set the control callback. This will be called
      // on every incoming ping, pong, and close frame.
      ws_.control_callback(
        std::bind(
          &websocket_session::on_control_callback,
          this,
          std::placeholders::_1,
          std::placeholders::_2));

      // Run the timer. The timer is operated
      // continuously, this simplifies the code.
      on_timer({});

      // Set the timer
      timer_.expires_after(std::chrono::seconds(15));

      uri_ = std::move(std::string(req.target().data(), req.target().size()));
      remote_= ws_.next_layer().remote_endpoint().address().to_string();

      // Accept the websocket handshake
      ws_.async_accept(
        req,
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &websocket_session::on_accept,
            shared_from_this(),
            std::placeholders::_1)));
    }

    void
      on_accept(boost::system::error_code ec)
    {
      // Happens when the timer closes the socket
      if (ec == boost::asio::error::operation_aborted)
        return;

      if (ec)
        return base::fail(ec, "accept");

      websocketList_->registerWebsocket(shared_from_this());

      // Read a message
      do_read();
    }

    // Called when the timer expires.
    void
      on_timer(boost::system::error_code ec)
    {
      if (ec && ec != boost::asio::error::operation_aborted)
        return base::fail(ec, "timer");

      // See if the timer really expired since the deadline may have moved.
      if (timer_.expiry() <= std::chrono::steady_clock::now())
      {
        // If this is the first time the timer expired,
        // send a ping to see if the other end is there.
        if (ws_.is_open() && ping_state_ == 0)
        {
          // Note that we are sending a ping
          ping_state_ = 1;

          // Set the timer
          timer_.expires_after(std::chrono::seconds(15));

          // Now send the ping
          ws_.async_ping({},
            boost::asio::bind_executor(
              strand_,
              std::bind(
                &websocket_session::on_ping,
                shared_from_this(),
                std::placeholders::_1)));
        }
        else
        {
          // The timer expired while trying to handshake,
          // or we sent a ping and it never completed or
          // we never got back a control frame, so close.

          // Closing the socket cancels all outstanding operations. They
          // will complete with boost::asio::error::operation_aborted
          ws_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
          ws_.next_layer().close(ec);
          return;
        }
      }

      // Wait on the timer
      timer_.async_wait(
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &websocket_session::on_timer,
            shared_from_this(),
            std::placeholders::_1)));
    }

    // Called to indicate activity from the remote peer
    void
      activity()
    {
      // Note that the connection is alive
      ping_state_ = 0;

      // Set the timer
      timer_.expires_after(std::chrono::seconds(15));
    }

    // Called after a ping is sent.
    void
      on_ping(boost::system::error_code ec)
    {
      // Happens when the timer closes the socket
      if (ec == boost::asio::error::operation_aborted)
        return;

      if (ec)
        return base::fail(ec, "ping");

      // Note that the ping was sent.
      if (ping_state_ == 1)
      {
        ping_state_ = 2;
      }
      else
      {
        // ping_state_ could have been set to 0
        // if an incoming control frame was received
        // at exactly the same time we sent a ping.
        BOOST_ASSERT(ping_state_ == 0);
      }
    }

    void
      on_control_callback(
        boost::beast::websocket::frame_type kind,
        boost::beast::string_view payload)
    {
      boost::ignore_unused(kind, payload);

      // Note that there is activity
      activity();
    }

    void
      do_read()
    {
      // Read a message into our buffer
      ws_.async_read(
        buffer_,
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &websocket_session::on_read,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
    }

    void
      on_read(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);

      // Happens when the timer closes the socket
      if (ec == boost::asio::error::operation_aborted)
        return;

      // This indicates that the websocket_session was closed
      if (ec == boost::beast::websocket::error::closed)
        return;

      if (ec)
        base::fail(ec, "read");

      // Note that there is activity
      activity();

      websocketList_->dataReceived(shared_from_this());
      buffer_.consume(buffer_.size());

      do_read();
      /*
      // Echo the message
      ws_.text(ws_.got_text());
      ws_.async_write(
        buffer_.data(),
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &websocket_session::on_write,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
      */
    }

    void send(const std::string& text)
    {
      {
        std::lock_guard<std::mutex> lock(fifoMutex);
        sendFifo.push(text);
      }

      do_write();
    }

    void do_write()
    {
      if (!writeInProgress)
        write_again();
    }

    void write_again()
    {
      std::lock_guard<std::mutex> lock(fifoMutex);
      writeInProgress = !sendFifo.empty();
      if (writeInProgress)
      {
        ws_.text(true);
        ws_.async_write(
          boost::asio::buffer(sendFifo.front()),
          boost::asio::bind_executor(
            strand_,
            std::bind(
              &websocket_session::on_write,
              shared_from_this(),
              std::placeholders::_1,
              std::placeholders::_2)));
      }
    }

    void
      on_write(
        boost::system::error_code ec,
        std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);

      // Happens when the timer closes the socket
      if (ec == boost::asio::error::operation_aborted)
        return;

      if (ec)
        return base::fail(ec, "write");

      // Clear the buffer
      buffer_.consume(buffer_.size());

      {
        std::lock_guard<std::mutex> lock(fifoMutex);
        sendFifo.pop();
      }

      write_again();
    }
  };

}

#endif // !_INCLUDE_WEBSOCKET_SESSION_H_

