#ifndef DATA_ACCESS_RANGE_HPP
#define DATA_ACCESS_RANGE_HPP


#include <cstddef>


class DataAccessRange {
private:
	//! The starting address of the data access
	void *_startAddress;
	
	//! For now we are not considering the length of the accesses
	
public:
	DataAccessRange(void *startAddress, __attribute__((unused)) size_t length)
		: _startAddress(startAddress)
	{
	}
	
	DataAccessRange()
		: _startAddress(0)
	{
	}
	
	bool operator<(DataAccessRange const &other) const
	{
		return _startAddress < other._startAddress;
	}
	
	bool operator==(DataAccessRange const &other) const
	{
		return _startAddress == other._startAddress;
	}
	
	bool operator!=(DataAccessRange const &other) const
	{
		return _startAddress != other._startAddress;
	}
	
};


#endif // DATA_ACCESS_RANGE_HPP
