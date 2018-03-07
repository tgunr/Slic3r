#ifndef slic3r_Bonjour_hpp_
#define slic3r_Bonjour_hpp_

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <boost/asio/ip/address.hpp>


namespace Slic3r {


struct BonjourReply
{
	boost::asio::ip::address ip;
	uint16_t port;
	std::string service_name;
	std::string hostname;
	std::string path;
	std::string version;

	BonjourReply(boost::asio::ip::address ip, uint16_t port, std::string service_name, std::string hostname);
};

std::ostream& operator<<(std::ostream &, const BonjourReply &);


/// Bonjour lookup performer
class Bonjour : public std::enable_shared_from_this<Bonjour> {
private:
	struct priv;
public:
	typedef std::shared_ptr<Bonjour> Ptr;
	typedef std::function<void(BonjourReply &&)> ReplyFn;
	typedef std::function<void()> CompleteFn;

	Bonjour(std::string service, std::string protocol = "tcp");
	Bonjour(Bonjour &&other);
	~Bonjour();

	Bonjour& set_timeout(unsigned timeout);
	Bonjour& on_reply(ReplyFn fn);
	Bonjour& on_complete(CompleteFn fn);

	Ptr lookup();
private:
	std::unique_ptr<priv> p;
};


}

#endif
