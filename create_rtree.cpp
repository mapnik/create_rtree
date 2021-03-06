/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2015 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <iostream>
#include <fstream>

#include <boost/timer/timer.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/support_multi_pass.hpp>
#include <boost/spirit/home/support/iterators/detail/functor_input_policy.hpp>
// boost.interprocess
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>

#include <mapnik/mapped_memory_cache.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>
// boost.iostreams
//#include <boost/iostreams/device/mapped_file.hpp>
// grammar
#include <mapnik/make_unique.hpp>
#include <mapnik/geometry.hpp>
#include <mapnik/geometry_adapters.hpp>
#include <mapnik/json/feature_collection_grammar.hpp>
#include <mapnik/json/extract_bounding_box_grammar_impl.hpp>
// mapnik

#include <boost/version.hpp>
#include <boost/geometry/index/rtree.hpp>

template <std::size_t Max, std::size_t Min>
struct geojson_linear : boost::geometry::index::linear<Max,Min> {};

namespace boost { namespace geometry { namespace index { namespace detail { namespace rtree {

template <std::size_t Max, std::size_t Min>
struct options_type<geojson_linear<Max,Min> >
{
    using type = options<geojson_linear<Max, Min>,
                         insert_default_tag,
                         choose_by_content_diff_tag,
                         split_default_tag,
                         linear_tag,
#if BOOST_VERSION >= 105700
                         node_variant_static_tag>;
#else
                         node_s_mem_static_tag>;

#endif
};

}}}}}

int main (int argc, char** argv)
{

    struct shm_remove
    {
        shm_remove() { boost::interprocess::shared_memory_object::remove("spatial-index"); }
        ~shm_remove(){ boost::interprocess::shared_memory_object::remove("spatial-index"); }
    } remover;

    // r-tree
    using box_type = mapnik::box2d<double>;
    using item_type = std::pair<box_type, std::pair<std::size_t, std::size_t> >;
    using allocator_type =  boost::interprocess::allocator<item_type, boost::interprocess::managed_mapped_file::segment_manager> ;
    using indexable_type = boost::geometry::index::indexable<item_type>;
    using equal_to_type = boost::geometry::index::equal_to<item_type>;
    //using spatial_index_type = boost::geometry::index::rtree<item_type, geojson_linear<16,4>>;
    using spatial_index_type = boost::geometry::index::rtree<item_type, geojson_linear<16,4>, indexable_type, equal_to_type, allocator_type>;
    // parser
    using base_iterator_type = char const*;
    const static mapnik::json::extract_bounding_box_grammar<base_iterator_type> bbox_grammar;

    std::cerr << "GeoJSON r-tree" << std::endl;

    if (argc != 2)
    {
        std::cerr << "Usage:" << argv[0] << " <json-file>" << std::endl;
        return EXIT_FAILURE;
    }

    // boost.interprocess
    boost::optional<mapnik::mapped_region_ptr> mapped_region =
        mapnik::mapped_memory_cache::instance().find(argv[1],true);
    if (!mapped_region)
    {
        throw std::runtime_error("could not create file mapping for "+ std::string(argv[1]));
    }

    char const* start = reinterpret_cast<char const*>((*mapped_region)->get_address());
    char const* end = start + (*mapped_region)->get_size();
    std::cerr << "file size = " << (*mapped_region)->get_size() << std::endl;
    {

        mapnik::json::boxes boxes;
        {
            boost::timer::auto_cpu_timer t;
            boost::spirit::standard::space_type space;
            bool result = boost::spirit::qi::phrase_parse(start, end, (bbox_grammar)(boost::phoenix::ref(boxes)) , space);
            if (!result)
            {
                std::cerr << "Failed to parse " << std::string(argv[1]) << std::endl;
                return EXIT_FAILURE;
            }
            std::cerr << "Count=" << boxes.size() << std::endl;
        }
#if 0
        {
            // bulk insert initialise r-tree
            std::unique_ptr<spatial_index_type> tree = std::make_unique<spatial_index_type>(boxes);
            std::cerr << "R-tree size= " << tree->size() << std::endl;
        }
#endif

#if 0
        {
            boost::timer::auto_cpu_timer t;
            std::string index_name = std::string(argv[1]) + ".index";
            boost::interprocess::managed_mapped_file index(boost::interprocess::open_or_create, index_name.c_str(), 64 * 1024 * 1024);
            allocator_type alloc(index.get_segment_manager());
            spatial_index_type * tree = index.find_or_construct<spatial_index_type>("spatial-index")(geojson_linear<16,4>(), indexable_type(), equal_to_type(), alloc);
            tree->clear();
            for (auto const& item : boxes)
            {
                tree->insert(item);
            }
            std::cerr << "R-tree size= " << tree->size() << std::endl;
        }
#endif

#if 1
        {
            boost::timer::auto_cpu_timer t;
            std::cerr << "size of item = " << sizeof(mapnik::json::boxes::value_type) << std::endl;
            static constexpr std::size_t magic_number = 64;
            std::size_t segment_size = magic_number * boxes.size();
            std::cerr << "segment size = " << segment_size << std::endl;
            boost::interprocess::managed_shared_memory segment(boost::interprocess::create_only, "spatial-index", segment_size);
            allocator_type alloc(segment.get_segment_manager());
            spatial_index_type * tree = segment.construct<spatial_index_type>("spatial-index")(std::move(boxes),
                                                                                               geojson_linear<16,4>(),
                                                                                               indexable_type(),
                                                                                               equal_to_type(), alloc);
            if (tree)
            {
                std::cerr << "R-tree size= " << tree->size() << std::endl;
                auto const& bounds = tree->bounds();
                mapnik::box2d<double> bbox(boost::geometry::get<0>(bounds.min_corner()),
                                           boost::geometry::get<1>(bounds.min_corner()),
                                           boost::geometry::get<0>(bounds.max_corner()),
                                           boost::geometry::get<1>(bounds.max_corner()));

                std::cerr << bbox << std::endl;
            }
        }
#endif
    }

    return EXIT_SUCCESS;
}
